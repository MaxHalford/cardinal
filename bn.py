"""Per-table Bayesian networks as base-relation estimators.

Each table gets a Chow-Liu BN over the columns the workload filters on.
Timestamps (as epoch seconds) and columns with many distinct values are
discretized into quantile bins; the others stay exact.
Join estimates keep DuckDB's formula, with BN base cardinalities:

    est(S) = Π card(R) / D(S)      D(S) = Π duckdb(R) / duckdb(S)

    uv run bn.py <db> <out>

Writes <out>/estimates/{bn_table,true_base}.jsonl. true_base uses true base
cardinalities: the best any per-table estimator can do with this formula.
"""

import collections
import json
import math
import pathlib
import sys
import time

import duckdb
import pandas as pd
import sorobn
import sqlglot
from sqlglot import exp

from ceq import read_jsonl, write_jsonl

OPS = {
    exp.EQ: sorobn.Eq,
    exp.NEQ: sorobn.Ne,
    exp.GT: sorobn.Gt,
    exp.GTE: sorobn.Ge,
    exp.LT: sorobn.Lt,
    exp.LTE: sorobn.Le,
}

# Columns with more distinct values than this are discretized.
MAX_STATES = 1000
N_BINS = 100

# Table filters that never change a result (see ceq.SKIP_FILTERS).
SKIP_FILTERS = {"OPTIONAL_FILTER", "DYNAMIC_FILTER", "BLOOM_FILTER"}


def conjuncts(node):
    node = node.unnest()
    if isinstance(node, exp.And):
        return conjuncts(node.left) + conjuncts(node.right)
    return [node]


def literal(node):
    """SQL literal -> Python value. Timestamps become epoch seconds."""
    if isinstance(node, exp.Cast) and node.to.is_type(*exp.DataType.TEMPORAL_TYPES):
        return pd.Timestamp(node.this.to_py()).timestamp()
    return node.to_py()


def predicates(filters):
    """Table filters -> {column: sorobn predicate}, ANDed per column.

    e.g. ['((r1.y > 2005.0) AND (r1.y < 2010.0))'] -> {'y': Gt(2005.0) & Lt(2010.0)}
    """
    preds = {}
    for f in filters:
        if f["type"] in SKIP_FILTERS:
            continue

        for node in conjuncts(sqlglot.parse_one(f["sql"], read="duckdb")):
            op = OPS.get(type(node))
            if op is None or not isinstance(node.this, exp.Column):
                raise ValueError(f"unsupported filter: {node.sql()}")

            col = node.this.name
            pred = op(literal(node.expression))
            preds[col] = preds[col] & pred if col in preds else pred
    return preds


def fit(con, table, cols):
    """Chow-Liu BN over cols of table, fitted on the full table."""
    exprs, schemes = [], {}
    for col in cols:
        typ, ndv = con.execute(f'SELECT typeof(any_value("{col}")), count(DISTINCT "{col}") FROM "{table}"').fetchone()
        temporal = typ.startswith("TIMESTAMP") or typ == "DATE"
        exprs.append(f'epoch("{col}") AS "{col}"' if temporal else f'"{col}"')
        if temporal or ndv > MAX_STATES:
            schemes[col] = sorobn.Discretizer(n_bins=N_BINS)
    df = con.execute(f'SELECT {", ".join(exprs)} FROM "{table}"').df()

    if len(cols) == 1:
        return sorobn.BayesNet(cols[0], discretizers=schemes).fit(df)

    # Structure learning needs discrete data: bin a copy.
    binned = df.copy()
    for col, scheme in schemes.items():
        binned[col] = scheme.fit_transform(df[col])
    return sorobn.BayesNet(*sorobn.structure.chow_liu(binned), discretizers=schemes).fit(df)


def combine(questions, duck, card):
    """Join estimates from base cardinalities, with DuckDB's denominator."""
    rows = []
    for q in questions:
        keys = [(q["query"], q["inv"], r) for r in q["set"]]
        if q["est"] == 0:
            rows.append(q | {"est": 0.0})
            continue

        denom = math.prod(duck[k] for k in keys) / q["est"]
        rows.append({k: q[k] for k in ("query", "inv", "set")} | {"est": math.prod(card[k] for k in keys) / denom})
    return rows


def main(db, out):
    out = pathlib.Path(out)
    rels = [r for r in read_jsonl(out / "relations.jsonl") if r["kind"] == "table"]
    questions = [q for q in read_jsonl(out / "questions.jsonl") if q["status"] == "ok"]
    con = duckdb.connect(db, read_only=True)

    # Columns each table is filtered on, across the workload.
    cols = collections.defaultdict(set)
    for r in rels:
        cols[r["table"]] |= set(predicates(r["table_filters"]))

    start = time.time()
    nets = {t: fit(con, t, sorted(c)) for t, c in cols.items() if c}
    print(f"fit {len(nets)} BNs in {time.time() - start:.1f}s", file=sys.stderr)

    # Base cardinalities: DuckDB's, BN's, and true.
    duck, bn, true = {}, {}, {}
    start = time.time()
    for r in rels:
        k = (r["query"], r["inv"], r["id"])
        preds = predicates(r["table_filters"])
        sel = nets[r["table"]].probability(preds) if preds else 1.0
        duck[k], bn[k] = r["est"], r["base_rows"] * sel
    print(f"{len(rels)} base estimates in {time.time() - start:.2f}s", file=sys.stderr)

    for q in questions:
        if len(q["set"]) == 1:
            true[(q["query"], q["inv"], q["set"][0])] = q["true"]

    (out / "estimates").mkdir(exist_ok=True)
    write_jsonl(out / "estimates/bn_table.jsonl", combine(questions, duck, bn))
    write_jsonl(out / "estimates/true_base.jsonl", combine(questions, duck, true))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
