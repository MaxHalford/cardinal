"""A recap of the extension against DuckDB alone, per benchmark, as a markdown table.

    uv run recap.py <out> ...

Reads ext.py plans (cardinal_off, cardinal_on) for estimate accuracy, and runtime.py
(cardinal_off, cardinal_on) for time.
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
                     for e in r["estimates"]} for m in ("off", "on")}
    keys = estimates["off"].keys() & estimates["on"].keys() & truth.keys()
    errors = sorted(max(max(estimates[mode][k], 1) / max(truth[k], 1), max(truth[k], 1) / max(estimates[mode][k], 1))
                    for k in keys)
    return errors[len(errors) // 2]


def row(out):
    out = pathlib.Path(out)
    times = {m: {r["query"]: r["seconds"] for r in read(out / f"runtime/cardinal_{m}.jsonl") if r["seconds"]}
             for m in ("off", "on")}
    queries = times["off"].keys() & times["on"].keys()
    off = sum(times["off"][q] for q in queries)
    fanout = sum(times["on"][q] for q in queries)
    faster = sum(1 for q in queries if times["off"][q] > RATIO * times["on"][q] and times["off"][q] - times["on"][q] > MIN_DELTA)
    slower = sum(1 for q in queries if times["on"][q] > RATIO * times["off"][q] and times["on"][q] - times["off"][q] > MIN_DELTA)
    speedup = math.exp(sum(math.log(times["off"][q] / times["on"][q]) for q in queries) / len(queries))
    return (f"| {out.name} | {len(queries)} | {off:.1f} s → {fanout:.1f} s | {off - fanout:+.1f} s ({100 * (off - fanout) / off:+.0f}%) "
            f"| {faster} / {len(queries) - faster - slower} / {slower} | {speedup:.2f}× | {median_qerror(out, 'off'):.1f} → {median_qerror(out, 'on'):.1f} |")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print("| Benchmark | Queries | Total time | Saved | Faster / same / slower | Geo-mean speedup | Median estimate error |")
    print("|---|---|---|---|---|---|---|")
    for out in sys.argv[1:]:
        print(row(out))
