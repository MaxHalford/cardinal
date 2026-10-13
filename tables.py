"""Per-table Bayesian networks: filter selectivities and fanout expectations.

Each table gets a Chow-Liu BN over the columns the workload filters on,
plus one fanout column per foreign key that references it:

    title.id <- cast_info.movie_id     title gets F(cast_info.movie_id):
                                       the cast_info rows of each movie

Columns with many distinct values are compacted: frequent values stay
exact, the rest share an OTHER state estimated with heuristics. Fanouts are
binned. Each bin holds about the same share of join output, so the heavy
tail that dominates join sizes keeps its resolution.

Table.expect answers E[1(filters) · Π F^k]: a selectivity when there are
no fanouts, an expected join size per row otherwise.
"""

import itertools
import pickle
import re

import numpy as np
import pandas as pd
import sorobn
import sqlglot
from sqlglot import exp

from ceq import COLUMN

OPS = {
    exp.EQ: sorobn.Eq,
    exp.NEQ: sorobn.Ne,
    exp.GT: sorobn.Gt,
    exp.GTE: sorobn.Ge,
    exp.LT: sorobn.Lt,
    exp.LTE: sorobn.Le,
}

# Columns with more distinct values than this are compacted.
MAX_STATES = 1000

# Inclusion-exclusion over more terms than this is too slow.
MAX_TERMS = 10

# Bins per fanout column, besides the exact zero bin.
FANOUT_BINS = 16

# Relation alias in filter SQL, e.g. r3."Score": irrelevant within a table.
ALIAS = re.compile(r"\br\d+\.")


def literal(node):
    """SQL literal -> Python value. Timestamps become epoch seconds."""
    if isinstance(node, exp.Cast) and node.to.is_type(*exp.DataType.TEMPORAL_TYPES):
        return pd.Timestamp(node.this.to_py()).timestamp()
    return node.to_py()


def predicate(node):
    """Expression over one column -> sorobn predicate."""
    node = node.unnest()

    if isinstance(node, exp.And):
        return predicate(node.left) & predicate(node.right)
    if isinstance(node, exp.Or):
        return predicate(node.left) | predicate(node.right)
    if isinstance(node, exp.Not):
        return ~predicate(node.this)

    if type(node) in OPS and isinstance(node.this, exp.Column):
        return OPS[type(node)](literal(node.expression))
    if isinstance(node, exp.In):
        return sorobn.In([literal(e) for e in node.expressions])
    if isinstance(node, exp.Is) and isinstance(node.expression, exp.Null):
        return sorobn.IsNull()
    if isinstance(node, exp.Like):
        pred = sorobn.Like(literal(node.expression))
        return ~pred if node.args.get("negate") else pred

    # DuckDB rewrites simple LIKE patterns into these functions.
    if isinstance(node, exp.Contains):
        return sorobn.Regex(re.escape(literal(node.expression)))
    if isinstance(node, exp.Anonymous) and node.name in ("prefix", "suffix"):
        text = re.escape(literal(node.expressions[1]))
        return sorobn.Regex("^" + text if node.name == "prefix" else text + "$")

    raise ValueError(f"unsupported filter: {node.sql()}")


def merge(a, b):
    """AND two conjunctions {column: predicate}."""
    out = dict(a)
    for col, pred in b.items():
        out[col] = out[col] & pred if col in out else pred
    return out


def dnf(node):
    """Expression -> list of conjunctions {column: predicate}, ORed.

    e.g. (g = 'm') OR (g = 'f' AND name LIKE 'A%') -> [{g: m}, {g: f, name: A%}]
    """
    node = node.unnest()
    cols = {c.name for c in node.find_all(exp.Column)}
    if len(cols) == 1:
        return [{cols.pop(): predicate(node)}]
    if isinstance(node, exp.And):
        return [merge(a, b) for a in dnf(node.left) for b in dnf(node.right)]
    if isinstance(node, exp.Or):
        return dnf(node.left) + dnf(node.right)
    raise ValueError(f"unsupported filter: {node.sql()}")


def compactor(limit):
    """'max_categories=1000,coverage=0.9' -> Compactor(max_categories=1000, coverage=0.9)."""
    kwargs = {}
    for part in limit.split(","):
        key, value = part.split("=")
        kwargs[key] = float(value) if "." in value else int(value)
    return sorobn.Compactor(**kwargs)


def bin_fanout(values):
    """Fanout counts -> (bin codes, mean count per bin).

    Bin 0 is exactly 0. The others split the total count into equal shares:
    e.g. one movie with 1,000 cast rows outweighs 500 movies with 1 each.
    A bin holds the counts up to its edge: (previous edge, edge].
    """
    values = np.asarray(values, dtype=np.int64)
    positive = np.sort(values[values > 0])
    if len(positive) == 0:
        return np.zeros(len(values), dtype=np.int64), np.array([0.0])

    # Few distinct counts: one exact bin each. Otherwise, edges at equal
    # shares of the cumulative count.
    edges = np.unique(positive)
    if len(edges) > FANOUT_BINS:
        share = np.cumsum(positive) / positive.sum()
        cuts = np.searchsorted(share, np.arange(1, FANOUT_BINS) / FANOUT_BINS)
        # Close each bin before the count that crosses its share: a dominant
        # count then starts a bin of its own.
        edges = np.unique(positive[np.maximum(cuts - 1, 0)])
    codes = np.where(values > 0, np.searchsorted(edges, values, side="left") + 1, 0)

    means = np.zeros(codes.max() + 1)
    sums = np.bincount(codes, weights=values, minlength=len(means))
    counts = np.bincount(codes, minlength=len(means))
    means[counts > 0] = sums[counts > 0] / counts[counts > 0]
    return codes, means


class Table:
    """A table's BN, its row count, and its fanout columns."""

    def __init__(self, net, rows, fanouts):
        self._net = net
        self.rows = rows
        # {(fk table, fk column): (BN node, mean count per bin)}
        self.fanouts = fanouts
        self._cache = {}

    def size(self):
        """Pickled size in bytes."""
        return len(pickle.dumps((self._net, self.fanouts)))

    def kept(self):
        """Frequent values kept by the compactors."""
        return sum(len(c.frequent_values_) for c in self._net.compactors.values())

    def expect(self, filters=(), powers=None):
        """E[1(all filters) · Π F^k] for powers {(fk table, fk column): k}."""
        powers = powers or {}
        key = (tuple(sorted(ALIAS.sub("", f) for f in filters)), tuple(sorted(powers.items())))
        if key not in self._cache:
            self._cache[key] = self._expect(filters, powers)
        return self._cache[key]

    def _expect(self, filters, powers):
        values = {}
        for fk, k in powers.items():
            node, means = self.fanouts[fk]
            values[node] = dict(enumerate(means ** k))

        terms = [{}]
        for sql in filters:
            terms = [merge(a, b) for a in terms for b in dnf(sqlglot.parse_one(sql, read="duckdb"))]
        if len(terms) > MAX_TERMS:
            raise ValueError(f"{len(terms)} DNF terms")

        # Inclusion-exclusion: E[1(A or B) w] = E[1(A) w] + E[1(B) w] - E[1(A and B) w].
        total = 0.0
        for k in range(1, len(terms) + 1):
            for subset in itertools.combinations(terms, k):
                event = {}
                for t in subset:
                    event = merge(event, t)
                total += (-1) ** (k + 1) * self._conjunction(event, values)
        return total

    def _conjunction(self, event, values):
        """E[1(event) · Π values] for one conjunction."""
        p = self._net.probability(event) if event else 1.0
        if not values or p == 0:
            return p
        return p * self._net.expectation(values, given=event)


def fit(con, table, cols, limit, fanouts=()):
    """Table model over filter cols and fanouts [(pk, fk table, fk column)].

    Fitted on the full table. Fanouts come from one GROUP BY per foreign key.
    """
    exprs, compactors = [], {}
    for col in cols:
        typ, ndv = con.execute(f'SELECT typeof(any_value("{col}")), count(DISTINCT "{col}") FROM "{table}"').fetchone()
        temporal = typ.startswith("TIMESTAMP") or typ == "DATE"
        exprs.append(f'epoch(t."{col}") AS "{col}"' if temporal else f't."{col}"')
        if ndv > MAX_STATES:
            compactors[col] = compactor(limit)

    # One LEFT JOIN per foreign key: each row of the table gets its count.
    joins = []
    for i, (pk, fk_table, fk_col) in enumerate(fanouts):
        exprs.append(f"coalesce(f{i}.n, 0) AS f{i}")
        joins.append(
            f' LEFT JOIN (SELECT "{fk_col}" AS k, count(*) AS n FROM "{fk_table}" '
            f'WHERE "{fk_col}" IS NOT NULL GROUP BY 1) f{i} ON t."{pk}" = f{i}.k'
        )

    # Shuffle: Compactor breaks frequency ties by first appearance, and the OTHER
    # heuristic needs frequent values that are representative, not the first
    # rows of a table stored in sorted order. Sort first: parallel joins return
    # rows in any order, which would make the shuffle, and the model, vary.
    sql = f'SELECT {", ".join(exprs)} FROM "{table}" t{"".join(joins)} ORDER BY t.rowid'
    df = con.execute(sql).df().sample(frac=1, random_state=0)

    nodes = {}
    for i, (pk, fk_table, fk_col) in enumerate(fanouts):
        node = f"F({fk_table}.{fk_col})"
        codes, means = bin_fanout(df.pop(f"f{i}"))
        df[node] = codes
        nodes[(fk_table, fk_col)] = (node, means)

    # Compactor only takes categoricals. Numbers stay numbers: ranges still work.
    for col in compactors:
        df[col] = df[col].astype("category")

    net = _structure(df, compactors).fit(df)
    return Table(net, len(df), nodes)


def _structure(df, compactors):
    """Chow-Liu tree. Structure learning needs few states: compact a copy."""
    if len(df.columns) == 1:
        return sorobn.BayesNet(df.columns[0], compactors=compactors)

    small = df.copy()
    for col, scheme in compactors.items():
        small[col] = scheme.fit_transform(df[col])
    return sorobn.BayesNet(*sorobn.structure.chow_liu(small), compactors=compactors)


def filter_columns(filters):
    """Columns a list of filter SQL strings use, e.g. ['(r1.x = 2)'] -> {'x'}."""
    return {col.strip('"') for sql in filters for _, col in COLUMN.findall(sql)}

