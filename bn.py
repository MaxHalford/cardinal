"""Bayesian-network estimators for the CE question dataset.

    uv run bn.py <db> <out> [limit]

Fits one model per table (tables.py) and writes, in <out>/estimates/:

    bn_<limit>      BN base cardinalities, DuckDB's join denominator:
                    est(S) = Π card(R) / D(S),  D(S) = Π duckdb(R) / duckdb(S)
    fanout_<limit>  BN joins through fanout columns (joins.py). Questions the
                    join estimator does not support fall back to bn_<limit>.
    true_base       true base cardinalities, DuckDB's join denominator: the
                    best any per-table estimator can do with that formula.

limit holds sorobn Compactor arguments for columns with more than 1000
distinct values, e.g. "max_categories=500,coverage=0.9". The default is
"max_categories=1000".
"""

import collections
import math
import pathlib
import sys
import time

import duckdb

import joins
import tables
from ceq import question_parts, read_jsonl, write_jsonl


def load_graphs(out):
    """{(query, inv): (relations sorted by id, edges)}."""
    graphs = collections.defaultdict(lambda: ([], []))
    for r in read_jsonl(out / "relations.jsonl"):
        graphs[(r["query"], r["inv"])][0].append(r)
    for e in read_jsonl(out / "edges.jsonl"):
        graphs[(e["query"], e["inv"])][1].append(e)
    for rels, _ in graphs.values():
        rels.sort(key=lambda r: r["id"])
    return dict(graphs)


def local_filters(graphs):
    """{(query, inv, relation): (table, base rows, DuckDB estimate, filter SQL)}."""
    result = {}
    for (q, inv), (rels, edges) in graphs.items():
        for r in rels:
            parts, status = question_parts({"filters": edges}, rels, [r["id"]])
            if status != "ok":
                continue
            result[(q, inv, r["id"])] = (r["table"], r["base_rows"], r["est"], parts[0][r["id"]])
    return result


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


def fit_tables(con, schema, locals_, limit):
    """{table: tables.Table}: filtered tables and the domains of foreign keys."""
    cols = collections.defaultdict(set)
    for table, _, _, filters in locals_.values():
        cols[table] |= tables.filter_columns(filters)
    fanouts = schema.fanouts()

    names = {t for t, c in cols.items() if c} | set(fanouts)
    return {t: tables.fit(con, t, sorted(cols[t]), limit, fanouts.get(t, ())) for t in sorted(names)}


def fanout_estimates(questions, graphs, estimator, fallback):
    """Join estimates through fanouts; unsupported questions use fallback."""
    rows, fell = [], 0
    for q, fb in zip(questions, fallback):
        rels, edges = graphs[(q["query"], q["inv"])]
        (local, preds), _ = question_parts({"filters": edges}, rels, q["set"])
        est = estimator.estimate({r["id"]: r for r in rels}, local, preds)
        if est is None:
            est, fell = fb["est"], fell + 1
        rows.append({k: q[k] for k in ("query", "inv", "set")} | {"est": est})
    return rows, fell


def main(db, out, limit=f"max_categories={tables.MAX_STATES}"):
    out = pathlib.Path(out)
    questions = [q for q in read_jsonl(out / "questions.jsonl") if q["status"] == "ok"]
    graphs = load_graphs(out)
    locals_ = local_filters(graphs)
    con = duckdb.connect(db, read_only=True)

    start = time.time()
    schema = joins.infer_schema(con, graphs.values())
    models = fit_tables(con, schema, locals_, limit)
    size = sum(m.size() for m in models.values())
    kept = sum(m.kept() for m in models.values())
    print(f"{len(schema.fk)} foreign keys, {len(models)} tables fitted in {time.time() - start:.1f}s, "
          f"{size / 1e6:.2f} MB, {kept} kept values", file=sys.stderr)

    # Base cardinalities: DuckDB's, BN's, and true.
    duck, bn, true = {}, {}, {}
    for k, (table, rows, est, filters) in locals_.items():
        duck[k], bn[k] = est, rows * (models[table].expect(filters) if filters else 1.0)
    for q in questions:
        if len(q["set"]) == 1:
            true[(q["query"], q["inv"], q["set"][0])] = q["true"]

    start = time.time()
    base = combine(questions, duck, bn)
    estimator = joins.JoinEstimator(schema, models)
    fanout, fell = fanout_estimates(questions, graphs, estimator, base)
    print(f"{len(questions)} estimates in {time.time() - start:.1f}s, {fell} fell back: {dict(estimator.unsupported)}", file=sys.stderr)

    (out / "estimates").mkdir(exist_ok=True)
    write_jsonl(out / f"estimates/bn_{limit}.jsonl", base)
    write_jsonl(out / f"estimates/fanout_{limit}.jsonl", fanout)
    write_jsonl(out / "estimates/true_base.jsonl", combine(questions, duck, true))


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
