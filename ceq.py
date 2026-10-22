"""Cardinality estimation questions (ceq): what DuckDB's join-order optimizer asks.

    uv run ceq.py trace  <workload> <db> <out>   run EXPLAIN with the trace patch
    uv run ceq.py build  <out>                   traces -> dataset tables (.jsonl)
    uv run ceq.py truth  <db> <out>              add true counts to questions
    uv run ceq.py check  <db> <out>              plans unchanged, full-join counts match

<workload> is a .sql file with one query per line, or a directory of .sql files.
A line may be "<true count>||<sql>" (STATS-CEB): check compares against it.
"""

import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import threading
import time

import duckdb
import sqlglot
from sqlglot import exp

ROOT = pathlib.Path(__file__).parent
PATCHED = ROOT / "duckdb/build/release/duckdb"
BASELINE = ROOT / "vendor/bin/duckdb-v2.0.0-baseline"
TIMEOUT = 600  # seconds per true count
TRUTH_WORKERS = 4  # true counts run at once

# Table filters that never change a result: hints, or runtime-only.
SKIP_FILTERS = {"OPTIONAL_FILTER", "DYNAMIC_FILTER", "BLOOM_FILTER"}

# A column in trace SQL: r<relation>.<column>, the column optionally quoted.
COLUMN = re.compile(r'\br(\d+)\.("(?:[^"]|"")+"|\w+)')


def read_workload(path):
    path = pathlib.Path(path)
    if path.is_dir():
        return [{"query": p.stem, "sql": p.read_text().strip()} for p in sorted(path.glob("*.sql"))]

    queries = []
    lines = [l.strip() for l in path.read_text().splitlines() if l.strip()]
    for i, line in enumerate(lines, 1):
        q = {"query": f"{i:03d}", "sql": line}
        if "||" in line:
            count, q["sql"] = line.split("||", 1)
            q["expected"] = int(count)
        queries.append(q)
    return queries


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f]


def write_jsonl(path, rows):
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


def explain(binary, db, sql, trace=None, inject=None):
    env = dict(os.environ)
    env.pop("DUCKDB_CE_TRACE", None)
    env.pop("DUCKDB_CE_INJECT", None)
    if trace:
        env["DUCKDB_CE_TRACE"] = str(trace)
    if inject:
        env["DUCKDB_CE_INJECT"] = str(inject)
    cmd = [str(binary), "-readonly", str(db), "-c", "EXPLAIN " + sql.rstrip(";")]
    return subprocess.run(cmd, env=env, check=True, capture_output=True, text=True).stdout


def trace(workload, db, out):
    out = pathlib.Path(out)
    (out / "traces").mkdir(parents=True, exist_ok=True)
    queries = read_workload(workload)
    for q in queries:
        path = out / "traces" / f"{q['query']}.jsonl"
        path.unlink(missing_ok=True)
        explain(PATCHED, db, q["sql"], trace=path)
    write_jsonl(out / "queries.jsonl", queries)


def question_parts(inv, rels, qset):
    """({relation: local predicates}, join predicates), or (None, status)."""
    for r in qset:
        if rels[r]["kind"] != "table":
            return None, "derived_relation"

    local = {r: list(rels[r]["filters"]) for r in qset}
    for r in qset:
        for f in rels[r]["table_filters"]:
            if f["type"] in SKIP_FILTERS:
                continue
            if f["sql"] is None:
                return None, "unrenderable_filter"
            local[r].append(f["sql"])

    joins = []
    for f in inv["filters"]:
        if not set(f["set"]) <= set(qset):
            continue
        if f["join_type"] != "INNER":
            return None, "join_type_" + f["join_type"].lower()
        if f["sql"] is None:
            return None, "unrenderable_filter"
        if len(f["set"]) == 1:
            local[f["set"][0]].append(f["sql"])
        else:
            joins.append(f["sql"])
    return (local, joins), "ok"


def table(rel):
    return f'"{rel["schema"]}"."{rel["table"]}" AS r{rel["id"]}'


def where(preds):
    return " WHERE " + " AND ".join(preds) if preds else ""


def plain_sql(rels, local, joins):
    """SELECT count(*) over the join: the readable, published form."""
    preds = [p for r in local for p in local[r]] + joins
    return f"SELECT count(*) FROM {', '.join(table(rels[r]) for r in local)}{where(preds)}"


def fast_sql(rels, local, joins):
    """Same count, without materializing the join.

    Each relation is grouped by the columns its join predicates use, then
    count(*) = sum of the product of group counts over the joined groups.
    Exact for any predicate over those columns.
    """
    if len(local) == 1:
        return plain_sql(rels, local, joins)

    cols = {r: set() for r in local}
    for j in joins:
        for r, col in COLUMN.findall(j):
            cols[int(r)].add(col)

    subs = []
    for r in local:
        keys = ", ".join(sorted(cols[r]))
        select = f"{keys}, count(*) AS c" if keys else "count(*) AS c"
        group = f" GROUP BY {keys}" if keys else ""
        subs.append(f"(SELECT {select} FROM {table(rels[r])}{where(local[r])}{group}) AS r{r}")

    product = " * ".join(f"r{r}.c" for r in local).replace(".c", ".c::HUGEINT", 1)
    return f"SELECT coalesce(sum({product}), 0) FROM {', '.join(subs)}{where(joins)}"


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
            parts, status = question_parts(inv, inv["relations"], e["set"])
            sql = plain_sql(inv["relations"], *parts) if parts else None
            fast = fast_sql(inv["relations"], *parts) if parts else None
            seen[k] = {"query": qid, "inv": e["inv"], "set": e["set"], "est": e["est"], "calls": 1,
                       "first_seq": e["seq"], "sql": sql, "fast_sql": fast, "status": status}
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
    fast = {q["sql"]: q["fast_sql"] for q in questions if q["status"] == "ok"}
    todo = sorted(fast.keys() - cache.keys())
    con = duckdb.connect(str(db), read_only=True)
    cursors = threading.local()

    # Counts are independent: run several at once, one cursor per thread.
    def count(sql):
        if not hasattr(cursors, "con"):
            cursors.con = con.cursor()
        return sql, run_count(cursors.con, fast[sql])

    with open(cache_path, "a") as f, concurrent.futures.ThreadPoolExecutor(TRUTH_WORKERS) as pool:
        for i, (sql, (n, secs)) in enumerate(pool.map(count, todo), 1):
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


def count_query(sql):
    """Replace the SELECT list by COUNT(*), or None if that changes the row count."""
    tree = sqlglot.parse_one(sql, read="duckdb")
    if not isinstance(tree, exp.Select) or tree.args.get("group") or tree.args.get("distinct"):
        return None
    return tree.select(exp.Count(this=exp.Star()), append=False).sql(dialect="duckdb")


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

    # 2. The full join's true count must equal the published count, or the
    #    query rerun as COUNT(*).
    n_rels = {}
    for r in rels:
        k = (r["query"], r["inv"])
        n_rels[k] = n_rels.get(k, 0) + 1
    for q in queries:
        sql = count_query(q["sql"])
        if sql is None:
            print(f"{q['query']}: skipped, no COUNT(*) form")
            continue
        full = [x for x in questions if x["query"] == q["query"] and len(x["set"]) == n_rels[(x["query"], x["inv"])] > 1]
        if len(full) != 1:
            print(f"{q['query']}: {len(full)} full-join questions")
            bad += 1
            continue
        if full[0]["status"] != "ok":
            print(f"{q['query']}: skipped, full join is {full[0]['status']}")
            continue
        actual = q["expected"] if "expected" in q else con.execute(sql).fetchone()[0]
        if full[0]["true"] != actual:
            print(f"{q['query']}: full join {full[0]['true']} != expected {actual}")
            bad += 1

    print(f"{len(queries)} queries, {bad} problems")


def main():
    cmds = {"trace": trace, "build": build, "truth": truth, "check": check}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](*sys.argv[2:])


if __name__ == "__main__":
    main()
