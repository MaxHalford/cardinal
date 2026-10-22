"""The cardinal extension on a benchmark: learn, then score its plans.

    uv run ext.py warmup <db> <out>
    uv run ext.py plans <db> <out> <mode> ...

warmup: one session plans every query of the workload, so the extension
learns its filtered columns and keys, then cardinal_fit_eager() saves models
and keys in the benchmark's store, <out>/cardinal.db. Later sessions start
from them, as a user's would.

plans: P-error of the plans DuckDB picks with the extension in each mode
(off, bn, fanout), costed with true cardinalities like plans.py. Each query
is planned twice in one traced process: the second plan is scored. The
extension's own work also shows in the trace: its queries, and the join
orders it compares on copies of the plan before DuckDB plans. So the dataset's
invocations are matched from the end, where DuckDB's real ones are.
Writes <out>/plans/cardinal_<mode>.jsonl.

runtime.py times the same modes: cardinal_<mode>.
"""

import collections
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile

import bn
import plans
from ceq import ROOT, read_jsonl, write_jsonl

EXTENSION = ROOT / "build/release/duckdb"

def settings(out):
    """The benchmark's own store, so evaluations leave ~/.duckdb alone."""
    return f"SET cardinal_store = '{pathlib.Path(out).resolve() / 'cardinal.db'}';"


def warmup(db, out):
    queries = read_jsonl(pathlib.Path(out) / "queries.jsonl")
    (pathlib.Path(out) / "cardinal.db").unlink(missing_ok=True)
    script = f"{settings(out)}\nSET cardinal_estimator = 'fanout';\n"
    script += "\n".join("EXPLAIN " + q["sql"].rstrip().rstrip(";") + ";" for q in queries)
    script += "\nSELECT count(*), round(sum(seconds), 2) FROM cardinal_fit_eager();\n"
    script += "SELECT count(*) FROM cardinal_models();\n"
    result = subprocess.run([str(EXTENSION), "-readonly", "-csv", "-noheader", str(db)], input=script, capture_output=True,
                            text=True, check=True)
    # The last two lines: the fit, then the keys.
    fitted, saved = result.stdout.strip().splitlines()[-2:]
    tables, seconds = fitted.split(",")
    print(f"{tables} tables fitted in {seconds} s, {saved} models saved", file=sys.stderr)


def trace(db, out, sql, mode):
    """Events of planning sql twice in one process."""
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "trace.jsonl"
        env = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_CE")}
        env["DUCKDB_CE_TRACE"] = str(path)
        explain = "EXPLAIN " + sql.rstrip().rstrip(";")
        cmd = [str(EXTENSION), "-readonly", str(db), "-cmd", f"{settings(out)} SET cardinal_estimator = '{mode}'",
               "-c", explain, "-c", explain]
        subprocess.run(cmd, env=env, check=True, capture_output=True, text=True)
        return read_jsonl(path)


def second_plans(events, expected):
    """{dataset invocation: plan tree} of the second planning, or None if not found."""
    tables = {e["inv"]: [r.get("table") for r in e["relations"]] for e in events if e["ev"] == "invocation"}
    trees = {e["inv"]: e["tree"] for e in events if e["ev"] == "plan"}
    ids = sorted(tables)
    second = ids[len(ids) // 2:]

    found, i = {}, len(second) - 1
    for inv, names in reversed(expected):
        while i >= 0 and tables[second[i]] != names:
            i -= 1
        if i < 0:
            return None
        if second[i] in trees:
            found[inv] = trees[second[i]]
        i -= 1
    return found


def score(db, out, modes):
    out = pathlib.Path(out)
    queries = read_jsonl(out / "queries.jsonl")
    graphs = bn.load_graphs(out)
    truth = plans.Truth(db, out, read_jsonl(out / "questions.jsonl"), graphs)
    best = {r["query"]: r for r in read_jsonl(out / "plans" / "true.jsonl")}
    expected = collections.defaultdict(list)
    for (q, inv), (rels, _) in sorted(graphs.items()):
        expected[q].append((inv, [r.get("table") for r in rels]))

    for mode in modes:
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            planned = list(pool.map(lambda q: second_plans(trace(db, out, q["sql"], mode), expected[q["query"]]), queries))

        rows = []
        for q, trees in zip(queries, planned):
            if trees is None:
                print(f"{q['query']}: invocations not found", file=sys.stderr)
                continue
            row = {"query": q["query"], "missing": 0, "plans": trees}
            for field, inner in (("cost", False), ("inner", True)):
                card = lambda inv: lambda s: truth(q["query"], inv, s)
                row[field] = sum(plans.cost(t, card(inv), inner) for inv, t in trees.items())
            if best[q["query"]]["cost"]:
                row["p_error"] = row["cost"] / best[q["query"]]["cost"]
            if best[q["query"]]["inner"]:
                row["inner_p_error"] = row["inner"] / best[q["query"]]["inner"]
            rows.append(row)
        plans.summary(f"cardinal_{mode}", rows)
        write_jsonl(out / "plans" / f"cardinal_{mode}.jsonl", rows)


if __name__ == "__main__":
    if len(sys.argv) < 4 or sys.argv[1] not in ("warmup", "plans"):
        sys.exit(__doc__)
    if sys.argv[1] == "warmup":
        warmup(*sys.argv[2:4])
    else:
        score(sys.argv[2], sys.argv[3], sys.argv[4:])
