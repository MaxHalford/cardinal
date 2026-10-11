"""Cardinality estimation questions (ceq): what DuckDB's join-order optimizer asks.

    uv run ceq.py trace  <workload> <db> <out>   run EXPLAIN with the trace patch
    uv run ceq.py build  <out>                   traces -> dataset tables (.jsonl)
    uv run ceq.py truth  <db> <out>              add true counts to questions
    uv run ceq.py check  <db> <out>              plans unchanged, full-join counts match

<workload> is a .sql file with one query per line, or a directory of .sql files.
"""

import json
import os
import pathlib
import subprocess
import sys
import threading
import time

import duckdb

ROOT = pathlib.Path(__file__).parent
PATCHED = ROOT / "vendor/duckdb/build/release/duckdb"
BASELINE = ROOT / "vendor/bin/duckdb-v1.5.6-baseline"
TIMEOUT = 600  # seconds per true count

# Table filters that never change a result: hints, or runtime-only.
SKIP_FILTERS = {"OPTIONAL_FILTER", "DYNAMIC_FILTER", "BLOOM_FILTER"}


def read_workload(path):
    path = pathlib.Path(path)
    if path.is_dir():
        return [(p.stem, p.read_text().strip()) for p in sorted(path.glob("*.sql"))]
    lines = [l.strip() for l in path.read_text().splitlines() if l.strip()]
    return [(f"{i:03d}", l) for i, l in enumerate(lines, 1)]


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f]


def write_jsonl(path, rows):
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


def explain(binary, db, sql, trace=None):
    env = dict(os.environ)
    env.pop("DUCKDB_CE_TRACE", None)
    if trace:
        env["DUCKDB_CE_TRACE"] = str(trace)
    cmd = [str(binary), "-readonly", str(db), "-c", "EXPLAIN " + sql.rstrip(";")]
    return subprocess.run(cmd, env=env, check=True, capture_output=True, text=True).stdout


def trace(workload, db, out):
    out = pathlib.Path(out)
    (out / "traces").mkdir(parents=True, exist_ok=True)
    queries = read_workload(workload)
    for qid, sql in queries:
        path = out / "traces" / f"{qid}.jsonl"
        path.unlink(missing_ok=True)
        explain(PATCHED, db, sql, trace=path)
    write_jsonl(out / "queries.jsonl", [{"query": q, "sql": s} for q, s in queries])


def count_sql(inv, rels, qset):
    """COUNT(*) SQL for relation set qset, or (None, status)."""
    for r in qset:
        if rels[r]["kind"] != "table":
            return None, "derived_relation"

    preds = []
    for r in qset:
        for f in rels[r]["table_filters"]:
            if f["type"] in SKIP_FILTERS:
                continue
            if f["sql"] is None:
                return None, "unrenderable_filter"
            preds.append(f["sql"])

    for f in inv["filters"]:
        if not set(f["set"]) <= set(qset):
            continue
        if f["join_type"] != "INNER":
            return None, "join_type_" + f["join_type"].lower()
        if f["sql"] is None:
            return None, "unrenderable_filter"
        preds.append(f["sql"])

    tables = ", ".join(f'"{rels[r]["schema"]}"."{rels[r]["table"]}" AS r{r}' for r in qset)
    where = " WHERE " + " AND ".join(preds) if preds else ""
    return f"SELECT count(*) FROM {tables}{where}", "ok"


def build(out):
    out = pathlib.Path(out)
    invs, rels_out, edges, questions = [], [], [], []

    for q in read_jsonl(out / "queries.jsonl"):
        qid = q["query"]
        events = read_jsonl(out / "traces" / f"{qid}.jsonl")
        by_inv = {e["inv"]: e for e in events if e["ev"] == "invocation"}

        for e in events:
            inv = by_inv.get(e["inv"])
            if e["ev"] == "phase":
                inv.setdefault("phases", []).append(e["name"])
            elif e["ev"] == "plan":
                inv["plan"] = e["tree"]
            elif e["ev"] == "tdom":
                inv.setdefault("tdoms", []).append({k: e[k] for k in ("columns", "filters", "has_hll", "hll", "no_hll")})

        for inv in by_inv.values():
            key = {"query": qid, "inv": inv["inv"]}
            invs.append(key | {k: inv.get(k) for k in ("parent", "depth", "reorderable", "phases", "tdoms", "plan")})
            rels_out += [key | r for r in inv["relations"]]
            edges += [key | f for f in inv["filters"]]

        seen = {}
        for e in events:
            if e["ev"] != "question":
                continue
            k = (e["inv"], tuple(e["set"]))
            if k in seen:
                seen[k]["calls"] += 1
                continue
            inv = by_inv[e["inv"]]
            sql, status = count_sql(inv, inv["relations"], e["set"])
            seen[k] = {"query": qid, "inv": e["inv"], "set": e["set"], "est": e["est"], "calls": 1,
                       "first_seq": e["seq"], "sql": sql, "status": status}
        questions += seen.values()

    write_jsonl(out / "invocations.jsonl", invs)
    write_jsonl(out / "relations.jsonl", rels_out)
    write_jsonl(out / "edges.jsonl", edges)
    write_jsonl(out / "questions.jsonl", questions)
    print(f"{len(questions)} questions, {sum(q['status'] == 'ok' for q in questions)} answerable")


def run_count(con, sql):
    """(count, seconds), or (None, seconds) on timeout."""
    timer = threading.Timer(TIMEOUT, con.interrupt)
    start = time.time()
    timer.start()
    try:
        return con.execute(sql).fetchone()[0], time.time() - start
    except duckdb.InterruptException:
        return None, time.time() - start
    finally:
        timer.cancel()


def truth(db, out):
    out = pathlib.Path(out)
    cache_path = out / "truth_cache.jsonl"
    cache = {r["sql"]: r for r in read_jsonl(cache_path)} if cache_path.exists() else {}
    questions = read_jsonl(out / "questions.jsonl")
    todo = sorted({q["sql"] for q in questions if q["status"] == "ok"} - cache.keys())
    con = duckdb.connect(str(db), read_only=True)

    with open(cache_path, "a") as f:
        for i, sql in enumerate(todo, 1):
            n, secs = run_count(con, sql)
            cache[sql] = {"sql": sql, "true": n, "seconds": round(secs, 3)}
            f.write(json.dumps(cache[sql]) + "\n")
            f.flush()
            print(f"\r{i}/{len(todo)}", end="", file=sys.stderr)

    for q in questions:
        hit = cache.get(q["sql"])
        q["true"] = hit["true"] if hit else None
        if hit and hit["true"] is None:
            q["status"] = "timeout"
    write_jsonl(out / "questions.jsonl", questions)


def check(db, out):
    out = pathlib.Path(out)
    queries = read_jsonl(out / "queries.jsonl")
    questions = read_jsonl(out / "questions.jsonl")
    rels = read_jsonl(out / "relations.jsonl")
    con = duckdb.connect(str(db), read_only=True)
    bad = 0

    # 1. The patch must not change plans.
    for q in queries:
        if explain(BASELINE, db, q["sql"]) != explain(PATCHED, db, q["sql"]):
            print(f"{q['query']}: plan differs from baseline")
            bad += 1

    # 2. The largest question of a COUNT(*) query must equal the query result.
    n_rels = {}
    for r in rels:
        k = (r["query"], r["inv"])
        n_rels[k] = n_rels.get(k, 0) + 1
    for q in queries:
        if not q["sql"].upper().startswith("SELECT COUNT(*)"):
            continue
        full = [x for x in questions if x["query"] == q["query"] and len(x["set"]) == n_rels[(x["query"], x["inv"])] > 1]
        if len(full) != 1:
            print(f"{q['query']}: {len(full)} full-join questions")
            bad += 1
            continue
        if full[0]["status"] != "ok":
            print(f"{q['query']}: skipped, full join is {full[0]['status']}")
            continue
        actual = con.execute(q["sql"]).fetchone()[0]
        if full[0]["true"] != actual:
            print(f"{q['query']}: full join {full[0]['true']} != query result {actual}")
            bad += 1

    print(f"{len(queries)} queries, {bad} problems")


def main():
    cmds = {"trace": trace, "build": build, "truth": truth, "check": check}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](*sys.argv[2:])


if __name__ == "__main__":
    main()
