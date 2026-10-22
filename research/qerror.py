"""Accuracy of the estimates DuckDB plans with: cardinal against DuckDB alone.

    uv run qerror.py <out> ...

Reads <out>/plans/cardinal_off.jsonl and cardinal_fanout.jsonl (ext.py plans), and
the true counts of <out>/questions.jsonl. Compares, per relation set DuckDB asked
about in both modes, q-error = max(estimate / true, true / estimate), both at least 1.
"""

import collections
import json
import pathlib
import sys

QUANTILES = (0.5, 0.9, 0.95, 0.99, 1.0)

# Sets by number of relations, up to this many; larger ones are grouped.
MAX_GROUP = 6

# A set is "worse" with cardinal when its q-error grows beyond this factor.
WORSE = 2.0


def read(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def estimates(path):
    """{(query, invocation, set): estimate}."""
    return {(r["query"], e["inv"], tuple(e["set"])): e["est"] for r in read(path) for e in r["estimates"]}


def qerror(est, true):
    est, true = max(est, 1.0), max(true, 1.0)
    return max(est / true, true / est)


def quantiles(values):
    values = sorted(values)
    return [values[min(int(q * len(values)), len(values) - 1)] for q in QUANTILES]


def report(out):
    out = pathlib.Path(out)
    truth = {(q["query"], q["inv"], tuple(q["set"])): q["true"] for q in read(out / "questions.jsonl")
             if q["status"] == "ok" and q["true"] is not None}
    off, fanout = estimates(out / "plans/cardinal_off.jsonl"), estimates(out / "plans/cardinal_fanout.jsonl")
    keys = sorted(off.keys() & fanout.keys() & truth.keys())

    groups = collections.defaultdict(lambda: ([], []))
    for k in keys:
        size = min(len(k[2]), MAX_GROUP)
        groups[size][0].append(qerror(off[k], truth[k]))
        groups[size][1].append(qerror(fanout[k], truth[k]))
        groups["all"][0].append(qerror(off[k], truth[k]))
        groups["all"][1].append(qerror(fanout[k], truth[k]))

    print(f"{out}: {len(keys)} sets")
    print(f"  {'relations':>9}  {'sets':>6}  {'DuckDB p50 / p90 / p95 / p99 / max':>44}  {'cardinal p50 / p90 / p95 / p99 / max':>44}")
    for size in [*sorted(s for s in groups if s != "all"), "all"]:
        a, b = groups[size]
        label = f"{size}+" if size == MAX_GROUP else str(size)
        fmt = lambda v: " / ".join(f"{x:.3g}" for x in quantiles(v))
        print(f"  {label:>9}  {len(a):>6}  {fmt(a):>44}  {fmt(b):>44}")

    worse = [k for k in keys if qerror(fanout[k], truth[k]) > WORSE * qerror(off[k], truth[k])]
    better = [k for k in keys if qerror(off[k], truth[k]) > WORSE * qerror(fanout[k], truth[k])]
    print(f"  q-error {WORSE:g}x better with cardinal: {len(better)} sets, {WORSE:g}x worse: {len(worse)} sets")
    for k in sorted(worse, key=lambda k: -qerror(fanout[k], truth[k]) / qerror(off[k], truth[k]))[:10]:
        print(f"    {k[0]} inv {k[1]} {list(k[2])}: true {truth[k]:.0f}, DuckDB {off[k]:.0f}, cardinal {fanout[k]:.0f}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for out in sys.argv[1:]:
        report(out)
