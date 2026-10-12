"""Query runtimes with injected estimates.

    uv run runtime.py <db> <out> <estimator> ...

Runs each query with the injection plans.py saved for that estimator
(<out>/plans/<estimator>/<query>.txt) and keeps the median of its timed
runs. Short queries get a warm-up and up to REPEATS runs. Long ones run
until BUDGET seconds: their noise is small relative to their length.
Writes <out>/runtime/<estimator>.jsonl and prints totals and speedups
over the first estimator.
"""

import math
import os
import pathlib
import re
import statistics
import subprocess
import sys

from ceq import PATCHED, read_jsonl, write_jsonl

REPEATS = 5

# Timed seconds per query: stop repeating beyond this.
BUDGET = 5.0

# Queries faster than this get a warm-up run.
WARM_UP = 1.0

# Seconds per run.
TIMEOUT = 600

# A query is slower or faster beyond 10% and this many seconds.
MIN_DELTA = 0.01

RUN_TIME = re.compile(r"Run Time \(s\): real ([\d.]+)")


def once(db, sql, inject):
    """One run in a fresh DuckDB process -> seconds, or None on timeout.

    Fresh: invocation ids, which key the injection, count from 1 per process.
    """
    env = dict(os.environ)
    env.pop("DUCKDB_CE_TRACE", None)
    env["DUCKDB_CE_INJECT"] = str(inject)
    cmd = [str(PATCHED), "-readonly", str(db), "-c", ".timer on", "-c", sql.rstrip(";")]
    try:
        result = subprocess.run(cmd, env=env, check=True, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    return float(RUN_TIME.search(result.stdout).group(1))


def measure(db, sql, inject):
    """Median runtime in seconds, or None on timeout.

    Short queries: a warm-up run (OS page cache), then up to REPEATS runs.
    Long queries: runs until BUDGET seconds, the first one included.
    """
    first = once(db, sql, inject)
    if first is None:
        return None
    times = [first] if first >= WARM_UP else []
    while len(times) < REPEATS and sum(times) < BUDGET:
        t = once(db, sql, inject)
        if t is None:
            return None
        times.append(t)
    return statistics.median(times)


def main(db, out, *names):
    out = pathlib.Path(out)
    queries = read_jsonl(out / "queries.jsonl")
    (out / "runtime").mkdir(exist_ok=True)

    results = {}
    for name in names:
        rows = []
        for q in queries:
            secs = measure(db, q["sql"], out / "plans" / name / f"{q['query']}.txt")
            rows.append({"query": q["query"], "seconds": secs})
            print(f"\r{name} {len(rows)}/{len(queries)}", end="", file=sys.stderr)
        print(file=sys.stderr)
        write_jsonl(out / f"runtime/{name}.jsonl", rows)
        results[name] = {r["query"]: r["seconds"] for r in rows}

    base = results[names[0]]
    print(f"{'estimator':<40} {'total s':>9} {'timeouts':>8} {'speedup':>8} {'slower':>6} {'faster':>6}")
    for name, secs in results.items():
        done = [q for q in secs if secs[q] is not None and base[q] is not None]
        ratios = [base[q] / secs[q] for q in done]
        geo = math.exp(statistics.mean(math.log(r) for r in ratios))
        total = sum(secs[q] for q in done)
        print(f"{name:<40} {total:>9.1f} {sum(s is None for s in secs.values()):>8} {geo:>8.2f} "
              f"{sum(secs[q] > 1.1 * base[q] and secs[q] - base[q] > MIN_DELTA for q in done):>6} "
              f"{sum(base[q] > 1.1 * secs[q] and base[q] - secs[q] > MIN_DELTA for q in done):>6}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
