"""Per-table Bayesian networks as base-relation estimators.

Each table gets a Chow-Liu BN over the columns the workload filters on.
Columns with many distinct values are compacted: frequent values stay
exact, the rest share an OTHER state estimated with heuristics.
Join estimates keep DuckDB's formula, with BN base cardinalities:

    est(S) = Π card(R) / D(S)      D(S) = Π duckdb(R) / duckdb(S)

    uv run bn.py <db> <out> [limit]

limit applies to columns with more than 1000 distinct values. An integer
caps their states (max_categories, default 1000). A fraction keeps values
covering at least that share of rows (min_frequency).

Writes <out>/estimates/bn_<limit>.jsonl and true_base.jsonl.
true_base uses true base cardinalities: the best any per-table estimator can
do with this formula.
"""

import collections
import itertools
import pickle
import math
import pathlib
import re
import sys
import time

import duckdb
import pandas as pd
import sorobn
import sqlglot
from sqlglot import exp

from ceq import COLUMN, question_parts, read_jsonl, write_jsonl

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


def probability(net, filters):
    """P(all filters), by inclusion-exclusion over their DNF terms."""
    terms = [{}]
    for sql in filters:
        terms = [merge(a, b) for a in terms for b in dnf(sqlglot.parse_one(sql, read="duckdb"))]
    if len(terms) > MAX_TERMS:
        raise ValueError(f"{len(terms)} DNF terms")

    total = 0.0
    for k in range(1, len(terms) + 1):
        for subset in itertools.combinations(terms, k):
            event = {}
            for t in subset:
                event = merge(event, t)
            total += (-1) ** (k + 1) * (net.probability(event) if event else 1.0)
    return total


def compactor(limit):
    if isinstance(limit, float):
        return sorobn.Compactor(min_frequency=limit)
    return sorobn.Compactor(max_categories=limit)


def fit(con, table, cols, limit):
    """Chow-Liu BN over cols of table, fitted on the full table."""
    exprs, compactors = [], {}
    for col in cols:
        typ, ndv = con.execute(f'SELECT typeof(any_value("{col}")), count(DISTINCT "{col}") FROM "{table}"').fetchone()
        temporal = typ.startswith("TIMESTAMP") or typ == "DATE"
        exprs.append(f'epoch("{col}") AS "{col}"' if temporal else f'"{col}"')
        if ndv > MAX_STATES:
            compactors[col] = compactor(limit)
    # Shuffle: Compactor breaks frequency ties by first appearance, and the OTHER
    # heuristic needs frequent values that are representative, not the first
    # rows of a table stored in sorted order.
    df = con.execute(f'SELECT {", ".join(exprs)} FROM "{table}"').df().sample(frac=1, random_state=0)

    # Compactor only takes categoricals. Numbers stay numbers: ranges still work.
    for col in compactors:
        df[col] = df[col].astype("category")

    if len(cols) == 1:
        return sorobn.BayesNet(cols[0], compactors=compactors).fit(df)

    # Structure learning needs few states: compact a copy.
    small = df.copy()
    for col, scheme in compactors.items():
        small[col] = scheme.fit_transform(df[col])
    return sorobn.BayesNet(*sorobn.structure.chow_liu(small), compactors=compactors).fit(df)


def combine(questions, duck, card):
    """Join estimates from base cardinalities, with DuckDB's denominator."""
    rows = []
    for q in questions:
        keys = [(q["query"], q["inv"], r) for r in q["set"]]
        est = 0.0
        if q["est"] > 0:
            denom = math.prod(duck[k] for k in keys) / q["est"]
            est = math.prod(card[k] for k in keys) / denom
        rows.append({k: q[k] for k in ("query", "inv", "set")} | {"est": est})
    return rows


def local_filters(out):
    """{(query, inv, relation): (table, base rows, DuckDB estimate, filter SQL)}."""
    graphs = collections.defaultdict(lambda: ([], []))
    for r in read_jsonl(out / "relations.jsonl"):
        graphs[(r["query"], r["inv"])][0].append(r)
    for e in read_jsonl(out / "edges.jsonl"):
        graphs[(e["query"], e["inv"])][1].append(e)

    result = {}
    for (q, inv), (rels, edges) in graphs.items():
        rels.sort(key=lambda r: r["id"])
        for r in rels:
            parts, status = question_parts({"filters": edges}, rels, [r["id"]])
            if status != "ok":
                continue
            result[(q, inv, r["id"])] = (r["table"], r["base_rows"], r["est"], parts[0][r["id"]])
    return result


def main(db, out, limit=str(MAX_STATES)):
    out = pathlib.Path(out)
    name = f"bn_{limit}"
    limit = float(limit) if "." in limit else int(limit)
    questions = [q for q in read_jsonl(out / "questions.jsonl") if q["status"] == "ok"]
    locals_ = local_filters(out)
    con = duckdb.connect(db, read_only=True)

    # Columns each table is filtered on, across the workload.
    cols = collections.defaultdict(set)
    for table, _, _, filters in locals_.values():
        for sql in filters:
            cols[table] |= {col.strip('"') for _, col in COLUMN.findall(sql)}

    start = time.time()
    nets = {t: fit(con, t, sorted(c), limit) for t, c in cols.items() if c}
    size = sum(len(pickle.dumps(n)) for n in nets.values())
    kept = sum(len(c.frequent_values_) for n in nets.values() for c in n.compactors.values())
    print(f"fit {len(nets)} BNs in {time.time() - start:.1f}s, {size / 1e6:.2f} MB, {kept} kept values", file=sys.stderr)

    # Base cardinalities: DuckDB's, BN's, and true.
    duck, bn, true = {}, {}, {}
    start = time.time()
    for k, (table, rows, est, filters) in locals_.items():
        sel = probability(nets[table], filters) if filters else 1.0
        duck[k], bn[k] = est, rows * sel
    print(f"{len(locals_)} base estimates in {time.time() - start:.2f}s", file=sys.stderr)

    for q in questions:
        if len(q["set"]) == 1:
            true[(q["query"], q["inv"], q["set"][0])] = q["true"]

    (out / "estimates").mkdir(exist_ok=True)
    write_jsonl(out / f"estimates/{name}.jsonl", combine(questions, duck, bn))
    write_jsonl(out / "estimates/true_base.jsonl", combine(questions, duck, true))


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
