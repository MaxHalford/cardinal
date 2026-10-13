"""Query runtimes with injected estimates.

    uv run runtime.py <db> <out> [--memory=<limit>] <estimator> ...

Runs each query with the injection plans.py saved for that estimator
(<out>/plans/<estimator>/<query>.txt) and keeps the median of its timed
runs. Short queries get a warm-up and up to REPEATS runs. Long ones run
until BUDGET seconds: their noise is small relative to their length.
Writes <out>/runtime/<estimator>.jsonl and prints totals and speedups
over the first estimator.

Estimators named cardinal_<mode> run the cardinal extension instead, with
SET cardinal_estimator = '<mode>' (e.g. cardinal_off, cardinal_bn): nothing
injected. An EXPLAIN first loads the models, outside the timed run.

--memory caps DuckDB's memory (memory_limit, e.g. 1GB): operators spill to
<db>.tmp instead. Results go to <out>/runtime/memory-<limit>/. A query that
fails, e.g. out of memory, counts as failed, like a timeout.
"""

import math
import os
import pathlib
import re
import statistics
import subprocess
import sys

from ceq import PATCHED, ROOT, read_jsonl, write_jsonl

REPEATS = 5

# Timed seconds per query: stop repeating beyond this.
BUDGET = 5.0

# Queries faster than this get a warm-up run.
WARM_UP = 1.0

# Seconds per run.
TIMEOUT = 600

# A query is slower or faster beyond 10% and this many seconds.
MIN_DELTA = 0.01

# DuckDB with the cardinal extension, and the prefix of its estimators.
EXTENSION = ROOT / "build/release/duckdb"
EXTENSION_PREFIX = "cardinal_"

RUN_TIME = re.compile(r"Run Time \(s\): real ([\d.]+)")


def once(db, sql, inject, memory=None, mode=None):
    """One run in a fresh DuckDB process -> (seconds, "ok"), or (None, "timeout" or "error").

    Fresh: invocation ids, which key the injection, count from 1 per process.
    mode: the cardinal extension's estimator, instead of an injection.
    """
    env = dict(os.environ)
    env.pop("DUCKDB_CE_TRACE", None)
    env.pop("DUCKDB_CE_INJECT", None)
    cmd = [str(EXTENSION if mode else PATCHED), "-readonly", str(db)]
    if memory:
        cmd += ["-c", f"SET memory_limit = '{memory}'"]
    if mode:
        cmd += ["-c", "SET cardinal_background = false", "-c", f"SET cardinal_estimator = '{mode}'",
                "-c", "EXPLAIN " + sql.rstrip(";")]
    else:
        env["DUCKDB_CE_INJECT"] = str(inject)
    cmd += ["-c", ".timer on", "-c", sql.rstrip(";")]
    try:
        result = subprocess.run(cmd, env=env, check=True, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    except subprocess.CalledProcessError:
        return None, "error"
    times = RUN_TIME.findall(result.stdout)
    # The CLI may exit 0 after an error: no timing then.
    return (float(times[-1]), "ok") if times and "Error" not in result.stderr else (None, "error")


def measure(db, sql, inject, memory=None, mode=None):
    """(median runtime in seconds, "ok"), or (None, why it failed).

    Short queries: a warm-up run (OS page cache), then up to REPEATS runs.
    Long queries: runs until BUDGET seconds, the first one included.
    """
    first, status = once(db, sql, inject, memory, mode)
    if first is None:
        return None, status
    times = [first] if first >= WARM_UP else []
    while len(times) < REPEATS and sum(times) < BUDGET:
        t, status = once(db, sql, inject, memory, mode)
        if t is None:
            return None, status
        times.append(t)
    return statistics.median(times), "ok"


def main(db, out, *names):
    out = pathlib.Path(out)
    memory = None
    if names and names[0].startswith("--memory="):
        memory, names = names[0].removeprefix("--memory="), names[1:]
    queries = read_jsonl(out / "queries.jsonl")
    results_dir = out / "runtime" / (f"memory-{memory}" if memory else "")
    results_dir.mkdir(parents=True, exist_ok=True)

    results = {}
    for name in names:
        rows = []
        for q in queries:
            mode = name.removeprefix(EXTENSION_PREFIX) if name.startswith(EXTENSION_PREFIX) else None
            secs, status = measure(db, q["sql"], out / "plans" / name / f"{q['query']}.txt", memory, mode)
            rows.append({"query": q["query"], "seconds": secs, "status": status})
            print(f"\r{name} {len(rows)}/{len(queries)}", end="", file=sys.stderr)
        print(file=sys.stderr)
        write_jsonl(results_dir / f"{name}.jsonl", rows)
        results[name] = {r["query"]: r["seconds"] for r in rows}

    base = results[names[0]]
    print(f"{'estimator':<40} {'total s':>9} {'failed':>8} {'speedup':>8} {'slower':>6} {'faster':>6}")
    for name, secs in results.items():
        done = [q for q in secs if secs[q] is not None and base[q] is not None]
        ratios = [base[q] / secs[q] for q in done]
        geo = math.exp(statistics.mean(math.log(r) for r in ratios)) if ratios else float("nan")
        total = sum(secs[q] for q in done)
        print(f"{name:<40} {total:>9.1f} {sum(s is None for s in secs.values()):>8} {geo:>8.2f} "
              f"{sum(secs[q] > 1.1 * base[q] and secs[q] - base[q] > MIN_DELTA for q in done):>6} "
              f"{sum(base[q] > 1.1 * secs[q] and base[q] - secs[q] > MIN_DELTA for q in done):>6}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
