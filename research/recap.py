"""A recap of the extension against DuckDB alone, per benchmark, as a markdown table.

    uv run recap.py <out> ...

Reads ext.py plans (cardinal_off, cardinal_fanout) for estimate accuracy, and runtime.py
(cardinal_off, cardinal_fanout) for time.
"""

import json
import math
import pathlib
import sys

# A query is faster or slower beyond this ratio and this many seconds, as in runtime.py.
RATIO = 1.1
MIN_DELTA = 0.01


def read(path):
    return [json.loads(line) for line in pathlib.Path(path).read_text().splitlines()]


def median_qerror(out, mode):
    """Median q-error of the estimates DuckDB plans with, over the sets both modes asked about."""
    truth = {(q["query"], q["inv"], tuple(q["set"])): q["true"] for q in read(out / "questions.jsonl")
             if q["status"] == "ok" and q["true"] is not None}
    estimates = {m: {(r["query"], e["inv"], tuple(e["set"])): e["est"] for r in read(out / f"plans/cardinal_{m}.jsonl")
                     for e in r["estimates"]} for m in ("off", "fanout")}
    keys = estimates["off"].keys() & estimates["fanout"].keys() & truth.keys()
    errors = sorted(max(max(estimates[mode][k], 1) / max(truth[k], 1), max(truth[k], 1) / max(estimates[mode][k], 1))
                    for k in keys)
    return errors[len(errors) // 2]


def row(out):
    out = pathlib.Path(out)
    times = {m: {r["query"]: r["seconds"] for r in read(out / f"runtime/cardinal_{m}.jsonl") if r["seconds"]}
             for m in ("off", "fanout")}
    queries = times["off"].keys() & times["fanout"].keys()
    off = sum(times["off"][q] for q in queries)
    fanout = sum(times["fanout"][q] for q in queries)
    faster = sum(1 for q in queries if times["off"][q] > RATIO * times["fanout"][q] and times["off"][q] - times["fanout"][q] > MIN_DELTA)
    slower = sum(1 for q in queries if times["fanout"][q] > RATIO * times["off"][q] and times["fanout"][q] - times["off"][q] > MIN_DELTA)
    speedup = math.exp(sum(math.log(times["off"][q] / times["fanout"][q]) for q in queries) / len(queries))
    return (f"| {out.name} | {len(queries)} | {off:.1f} s → {fanout:.1f} s | {off - fanout:+.1f} s ({100 * (off - fanout) / off:+.0f}%) "
            f"| {faster} / {slower} | {speedup:.2f}× | {median_qerror(out, 'off'):.1f} → {median_qerror(out, 'fanout'):.1f} |")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print("| Benchmark | Queries | Total time | Saved | Faster / slower | Geo-mean speedup | Median estimate error |")
    print("|---|---|---|---|---|---|---|")
    for out in sys.argv[1:]:
        print(row(out))
