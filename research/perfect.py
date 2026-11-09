"""Summarize runtimes with DuckDB and perfect cardinality estimates.

    uv run perfect.py <out> ...

Reads plans/{duckdb,true}.jsonl and runtime/{duckdb,true}.jsonl. A runtime
changes when it differs by more than 10% and 10 ms, as in runtime.py.
"""

import json
import math
import pathlib
import sys

RATIO = 1.1
MIN_DELTA = 0.01


def read(path):
    return [json.loads(line) for line in pathlib.Path(path).read_text().splitlines()]


def times(out, name):
    return {row["query"]: row["seconds"] for row in read(out / f"runtime/{name}.jsonl")
            if row.get("status", "ok") == "ok" and row["seconds"] is not None}


def changed_plans(out):
    plans = {
        name: {row["query"]: row["plans"] for row in read(out / f"plans/{name}.jsonl")}
        for name in ("duckdb", "true")
    }
    queries = plans["duckdb"].keys() & plans["true"].keys()
    return sum(plans["duckdb"][query] != plans["true"][query] for query in queries)


def summarize(out):
    out = pathlib.Path(out)
    runtime = {name: times(out, name) for name in ("duckdb", "true")}
    queries = runtime["duckdb"].keys() & runtime["true"].keys()
    faster = sum(runtime["duckdb"][query] > RATIO * runtime["true"][query]
                 and runtime["duckdb"][query] - runtime["true"][query] > MIN_DELTA
                 for query in queries)
    slower = sum(runtime["true"][query] > RATIO * runtime["duckdb"][query]
                 and runtime["true"][query] - runtime["duckdb"][query] > MIN_DELTA
                 for query in queries)
    ratios = [runtime["duckdb"][query] / runtime["true"][query] for query in queries]

    return {
        "benchmark": out.name,
        "queries": len(queries),
        "plans": changed_plans(out),
        "duckdb": sum(runtime["duckdb"][query] for query in queries),
        "true": sum(runtime["true"][query] for query in queries),
        "faster": faster,
        "same": len(queries) - faster - slower,
        "slower": slower,
        "speedup": math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios)),
    }


def row(summary):
    return (f"| {summary['benchmark']} | {summary['queries']} | {summary['plans']} | "
            f"{summary['duckdb']:.1f} s → {summary['true']:.1f} s | "
            f"{summary['faster']} / {summary['same']} / {summary['slower']} | "
            f"{summary['speedup']:.2f}× |")


def main(*outputs):
    print("| Benchmark | Queries | Changed plans | Total time | Faster / same / slower | Geo-mean speedup |")
    print("|---|---:|---:|---:|---:|---:|")
    for out in outputs:
        print(row(summarize(out)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(*sys.argv[1:])
