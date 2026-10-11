"""q-error of DuckDB and other estimators, by number of relations.

    uv run score.py <out> [name ...]    name: <out>/estimates/<name>.jsonl

q-error = max(e/t, t/e), both clamped to >= 1.
"""

import pathlib
import statistics
import sys

from ceq import read_jsonl

QUANTILES = (0.9, 0.95, 0.99)


def qerror(est, true):
    est, true = max(est, 1), max(true, 1)
    return max(est / true, true / est)


def summary(errors):
    errors = sorted(errors)
    pick = lambda q: errors[min(len(errors) - 1, int(q * len(errors)))]
    return [statistics.median(errors)] + [pick(q) for q in QUANTILES] + [errors[-1]]


def main(out, *names):
    out = pathlib.Path(out)
    questions = [q for q in read_jsonl(out / "questions.jsonl") if q["status"] == "ok"]
    true = {(q["query"], q["inv"], tuple(q["set"])): q["true"] for q in questions}

    estimators = {"duckdb": {k: q["est"] for k, q in zip(true, questions)}}
    for name in names:
        rows = read_jsonl(out / "estimates" / f"{name}.jsonl")
        estimators[name] = {(r["query"], r["inv"], tuple(r["set"])): r["est"] for r in rows}

    sizes = sorted({len(k[2]) for k in true})
    print(f"{'estimator':<12} {'rels':>4} {'n':>5} {'median':>9} {'p90':>9} {'p95':>9} {'p99':>9} {'max':>10}")
    for name, est in estimators.items():
        for size in sizes:
            keys = [k for k in true if len(k[2]) == size]
            stats = summary(qerror(est[k], true[k]) for k in keys)
            print(f"{name:<12} {size:>4} {len(keys):>5} " + " ".join(f"{s:>9.1f}" for s in stats[:-1]) + f" {stats[-1]:>10.1f}")
        print()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(*sys.argv[1:])
