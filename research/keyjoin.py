"""Key joins through groups of key values: which join assumption holds best?

    uv run keyjoin.py <db> <out> [queries]

Each table is summarized exactly, without a Bayesian network: only the
assumption about which key a foreign-key row references varies.

Each foreign-key row is summarized by the group of the key it references.
Given its group, the row references a key v of the group with probability:

    G (uniform)   1 / keys(g)          the user's value classes
    H (fanout)    F(v) / M(g)          fanout, its selectivity by group

F(v): rows of the foreign-key table referencing v. M(g): those of group g.
The domain row of a class keeps its key, so its filter and its fanouts are
exact, as in a per-table model with fanout columns.

    groups  one        H: today's fanout estimator, G: DuckDB's uniformity
            key        exact for a tree of joins: a check
            topK       the K keys of most join weight alone, the rest shared
            massN      N groups of keys by join weight, each of equal mass
            ownN       as massN, by each foreign key's own fanout

Small filtered dimensions are folded, as the extension does: the foreign key
of a dimension of at most 10,000 rows gets exact keys.

Prints q-error percentiles per method, and per number of relations. The
bn row: bn.py's estimates, today's assumption on Bayesian networks. The
extension row: the estimates DuckDB planned with (ext.py plans).
"""

import collections
import multiprocessing
import pathlib
import re
import sys
import time

import duckdb
import numpy as np

import bn
import joins
from ceq import question_parts, read_jsonl

# Dimensions of at most this many rows are folded, as in the extension.
MAX_DIMENSION_ROWS = 10_000

# Processes: per-query work is independent.
WORKERS = 8

ALIAS = re.compile(r"\br\d+\.")

METHODS = [
    ("exact", "G", "key"),
    ("uniform", "G", "one"),
    ("today", "H", "one"),
    ("today-nofold", "H", "one"),
    ("G-top100", "G", "top100"),
    ("G-top1000", "G", "top1000"),
    ("G-top10000", "G", "top10000"),
    ("G-mass16", "G", "mass16"),
    ("G-mass64", "G", "mass64"),
    ("G-mass256", "G", "mass256"),
    ("G-own16", "G", "own16"),
    ("G-own64", "G", "own64"),
    ("H-mass4", "H", "mass4"),
    ("H-mass16", "H", "mass16"),
    ("H-mass64", "H", "mass64"),
    ("H-mass256", "H", "mass256"),
    ("H-own16", "H", "own16"),
    ("H-own64", "H", "own64"),
    ("H-own256", "H", "own256"),
]


def bins(x, n):
    """Keys -> groups of about equal mass Σx, ties together, zeros apart.

    e.g. x = [0, 1, 1, 8], n = 2 -> [0, 1, 1, 2]: 1 + 1 and 8 split the mass.
    """
    values, inverse = np.unique(x, return_inverse=True)
    mass = values * np.bincount(inverse)
    start = np.cumsum(mass) - mass
    total = mass.sum()
    group = np.where(values > 0, np.floor(start / max(total, 1) * n) + 1, 0)
    _, dense = np.unique(group, return_inverse=True)
    return dense[inverse]


class Domain:
    """A primary key: its sorted values, and the fanout of each foreign key."""

    def __init__(self, con, table, column, fk_columns):
        self.keys = np.array([v for (v,) in con.execute(
            f'SELECT "{column}" FROM "{table}" ORDER BY 1').fetchall()])
        self.fanout = {}
        for fk in fk_columns:
            t, c = fk
            rows = con.execute(f'SELECT "{c}", count(*) FROM "{t}" WHERE "{c}" IS NOT NULL GROUP BY 1').fetchall()
            idx, counts = self.index(np.array([r[0] for r in rows]), np.array([r[1] for r in rows]))
            f = np.zeros(len(self.keys))
            f[idx] = counts
            self.fanout[fk] = f
        self.weight = 1 + sum(self.fanout.values())
        self._groups = {}

    def index(self, values, *columns):
        """Values -> their key positions; orphans dropped, with the same rows of columns."""
        i = np.searchsorted(self.keys, values).clip(0, len(self.keys) - 1)
        ok = self.keys[i] == values
        return (i[ok], *(c[ok] for c in columns))

    def groups(self, spec, fk):
        """Group of each key for foreign key fk, e.g. spec 'top100'."""
        k = (spec, fk if spec.startswith("own") else None)
        if k in self._groups:
            return self._groups[k]
        n = len(self.keys)
        if spec == "one":
            g = np.zeros(n, dtype=np.int64)
        elif spec == "key":
            g = np.arange(n)
        elif spec.startswith("top"):
            top = np.argsort(-self.weight, kind="stable")[: int(spec[3:])]
            g = np.zeros(n, dtype=np.int64)
            g[top] = np.arange(1, len(top) + 1)
        elif spec.startswith("mass"):
            g = bins(self.weight, int(spec[4:]))
        elif spec.startswith("own"):
            g = bins(self.fanout[fk], int(spec[3:]))
        else:
            raise ValueError(spec)
        self._groups[k] = g
        return g

    def probability(self, kind, spec, fk):
        """P(v | g(v)) for each key v: a foreign-key row of group g references v."""
        k = (kind, spec, fk)
        if k in self._groups:
            return self._groups[k]
        g = self.groups(spec, fk)
        if kind == "G":
            p = 1.0 / np.bincount(g)[g]
        else:
            f = self.fanout[fk]
            m = np.bincount(g, weights=f)[g]
            p = np.divide(f, m, out=np.zeros_like(f), where=m > 0)
        self._groups[k] = p
        return p


class Oracle:
    """Estimates of one question under one method, from exact per-table counts."""

    def __init__(self, con, schema):
        self.con, self.schema = con, schema
        self.domains = {}
        self.cache = {}

    def domain(self, key):
        if key not in self.domains:
            fks = sorted(fk for fk, ref in self.schema.fk.items() if ref == key)
            self.domains[key] = Domain(self.con, *key, fks)
        return self.domains[key]

    def counts(self, table, filters, columns):
        """Rows passing filters, counted by the values of columns."""
        filters = tuple(sorted(ALIAS.sub("t.", f) for f in filters))
        k = (table, filters, columns)
        if k not in self.cache:
            preds = list(filters) + [f't."{c}" IS NOT NULL' for c in columns]
            cols = ", ".join(f't."{c}"' for c in columns)
            sql = f'SELECT {cols}, count(*) FROM "{table}" AS t WHERE {" AND ".join(preds)} GROUP BY ALL'
            rows = self.con.execute(sql).fetchnumpy()
            self.cache[k] = [np.asarray(v) for v in rows.values()]
        return self.cache[k]

    def positions(self, table, filters, columns, i, dom):
        """Key positions of column i of counts, and whether each is a key."""
        k = (table, tuple(sorted(ALIAS.sub("t.", f) for f in filters)), columns, i)
        if k not in self.cache:
            v = self.counts(table, filters, columns)[i]
            pos = np.searchsorted(dom.keys, v).clip(0, len(dom.keys) - 1)
            self.cache[k] = (pos, dom.keys[pos] == v)
        return self.cache[k]

    def estimate(self, rels, local, classes, kind, spec, fold):
        self.rels, self.local, self.kind = rels, local, kind
        self.classes = classes
        self.of = collections.defaultdict(list)
        self.info = {}
        for c in classes:
            members = list(c.fks) + ([c.d] if c.d is not None else [])
            for m in members:
                self.of[m].append(c)
            fk = next(iter(c.fks.items()))
            key = self.schema.fk[(rels[fk[0]]["table"], fk[1])]
            dom = self.domain(key)
            folded = fold and self._folds(c, key)
            groups = {}
            for m, col in c.fks.items():
                fkey = (rels[m]["table"], col)
                s = "key" if folded else spec
                groups[m] = (fkey, dom.groups(s, fkey), dom.probability(kind, s, fkey))
            self.info[id(c)] = (key, dom, groups)

        root = min(local)
        return self._relation(root, None).sum()

    def _folds(self, c, key):
        """A small filtered dimension, joined by its foreign key alone."""
        return (c.d is not None and len(c.fks) == 1 and self.local[c.d]
                and len(self.of[c.d]) == 1 and self.schema.rows[key[0]] <= MAX_DIMENSION_ROWS)

    def _column(self, r, c):
        if r == c.d:
            return self.info[id(c)][0][1]
        return c.fks[r]

    def _relation(self, r, parent):
        """Rows of r weighted by their matches away from class parent, by parent attribute."""
        classes = self.of[r]
        columns = tuple(self._column(r, c) for c in classes)
        values = self.counts(self.rels[r]["table"], self.local[r], columns)
        w = values[-1].astype(float)

        attrs = []
        for j, c in enumerate(classes):
            _, dom, groups = self.info[id(c)]
            i, ok = self.positions(self.rels[r]["table"], self.local[r], columns, j, dom)
            w = np.where(ok, w, 0)
            attrs.append(i if r == c.d else groups[r][1][i])

        target = None
        for c, a in zip(classes, attrs):
            if c is parent:
                target = a
                continue
            w = w * self._class(c, r)[a]
        if target is None:
            return w
        size = len(self.info[id(parent)][1].keys) if r == parent.d else self.info[id(parent)][2][r][1].max() + 1
        return np.bincount(target, weights=w, minlength=size)

    def _class(self, c, parent):
        """Matches per row of parent through class c, by parent attribute."""
        _, dom, groups = self.info[id(c)]
        prod = np.ones(len(dom.keys))
        if c.d is not None and c.d != parent:
            prod *= self._relation(c.d, c)
        for m, (_, g, p) in groups.items():
            if m != parent:
                prod *= self._relation(m, c)[g] * p
        if parent == c.d:
            return prod
        _, g, p = groups[parent]
        return np.bincount(g, weights=p * prod, minlength=g.max() + 1)


def qerror(est, true):
    est, true = max(est, 1.0), max(true, 1.0)
    return max(est / true, true / est)


def report(errors, label):
    print(f"\n{label}")
    print(f"{'method':<14} {'n':>6} {'p50':>8} {'p90':>9} {'p99':>10} {'max':>10} {'gmean':>7}  vs today: 2x better / 2x worse")
    for name, e in errors.items():
        e = np.array(e)
        # The extension's estimates cover only the sets DuckDB planned with.
        today = np.array(errors["today"])[~np.isnan(e)]
        e = e[~np.isnan(e)]
        if not len(e):
            continue
        p = np.percentile(e, [50, 90, 99])
        better = int(np.sum(today / e >= 2))
        worse = int(np.sum(e / today >= 2))
        print(f"{name:<14} {len(e):>6} {p[0]:>8.3g} {p[1]:>9.3g} {p[2]:>10.3g} {e.max():>10.3g} "
              f"{np.exp(np.log(e).mean()):>7.3g}  {better:>5} / {worse}")


# Per worker process: the database, the schema and the oracle.
_worker = {}


def _init(db, out):
    con = duckdb.connect(db, read_only=True)
    graphs = bn.load_graphs(pathlib.Path(out))
    schema = joins.infer_schema(con, graphs.values())
    plans = pathlib.Path(out) / "plans/cardinal_fanout.jsonl"
    extension = {}
    if plans.exists():
        for r in read_jsonl(plans):
            for e in r["estimates"]:
                extension[(r["query"], e["inv"], tuple(e["set"]))] = e["est"]
    # Python BN estimates through fanouts (bn.py), without the extension's formula fit.
    python = pathlib.Path(out) / "estimates/fanout_max_categories=1000.jsonl"
    bn_estimates = {}
    if python.exists():
        bn_estimates = {(e["query"], e["inv"], tuple(e["set"])): e["est"] for e in read_jsonl(python)}
    _worker.update(bn=bn_estimates, extension=extension, graphs=graphs, parser=joins.JoinEstimator(schema, {}), oracle=Oracle(con, schema))


def _query(questions):
    """q-errors of one query's questions: [(relations, {method: q-error})], skipped, mismatches."""
    graphs, parser, oracle = _worker["graphs"], _worker["parser"], _worker["oracle"]
    oracle.cache.clear()
    rows, skipped, mismatch = [], collections.Counter(), []
    for q in questions:
        rels, edges = graphs[(q["query"], q["inv"])]
        (local, preds), _ = question_parts({"filters": edges}, rels, q["set"])
        rels = {r["id"]: r for r in rels}
        classes = parser._classes(rels, preds)
        if classes is None:
            skipped["unsupported"] += 1
            continue
        if not joins._is_tree(list(local), classes):
            skipped["cycle"] += 1
            continue

        errors = {}
        for name, kind, spec in METHODS:
            est = oracle.estimate(rels, local, classes, kind, spec, fold=name != "today-nofold")
            errors[name] = qerror(est, q["true"])
        k = (q["query"], q["inv"], tuple(q["set"]))
        for name in ("bn", "extension"):
            est = _worker[name].get(k)
            errors[name] = float("nan") if est is None else qerror(est, q["true"])
        if errors["exact"] > 1.0001:
            mismatch.append((q["query"], q["set"], q["true"]))
        rows.append((len(q["set"]), errors))
    return rows, skipped, mismatch


def main(db, out, queries=None, workers=WORKERS):
    keep = set(queries.split(",")) if queries else None
    by_query = collections.defaultdict(list)
    for q in read_jsonl(pathlib.Path(out) / "questions.jsonl"):
        if q["status"] == "ok" and len(q["set"]) > 1 and (keep is None or q["query"] in keep):
            by_query[q["query"]].append(q)

    names = [name for name, _, _ in METHODS] + ["bn", "extension"]
    errors = {name: [] for name in names}
    by_size = collections.defaultdict(lambda: {name: [] for name in names})
    skipped, mismatch = collections.Counter(), []
    start = time.time()
    with multiprocessing.Pool(workers, _init, (db, out)) as pool:
        for done, (rows, s, m) in enumerate(pool.imap_unordered(_query, by_query.values()), 1):
            print(f"{done}/{len(by_query)} queries, {time.time() - start:.0f}s", file=sys.stderr, flush=True)
            skipped += s
            mismatch += m
            for size, e in rows:
                for name, value in e.items():
                    errors[name].append(value)
                    by_size[min(size, 5)][name].append(value)

    print(f"{out}: {len(errors['exact'])} questions in {time.time() - start:.0f}s, skipped {dict(skipped)}")
    if mismatch:
        print(f"exact differs from truth on {len(mismatch)}: {mismatch[:5]}")
    report(errors, "all")
    for size in sorted(by_size):
        report(by_size[size], f"{size}{'+' if size == 5 else ''} relations")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
