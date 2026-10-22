"""DuckDB's join formula, fitted per query to fanout estimates: what the
cardinal extension can make DuckDB estimate, through row and distinct counts.

    uv run fitjoin.py <out> <estimates> <new name> [flat|size]

DuckDB estimates a set S of relations as

    log est(S) = Σ_{R ∈ S} n_R − Σ_{edges joining S} d_class − log(1 + unused)

n_R: log rows of R after filters. d_c: log distinct values of join class c.
Edges join S in decreasing d order; once S is connected, each class with
edges left counts as unused. Per query, n and d are fitted (least squares)
to the estimates of <out>/estimates/<estimates>.jsonl, and the formula's
values written to <out>/estimates/<new name>.jsonl, for plans.py and
runtime.py. size: the fit weighs a set by its number of relations.

Checks first that the formula, with DuckDB's own inputs, gives DuckDB's own
estimates.
"""

import collections
import math
import pathlib
import sys

import numpy as np

from ceq import read_jsonl, write_jsonl


def load_classes(out):
    """{(query, inv): [(filter indexes, DuckDB's tdom)]}, from the traces."""
    classes = collections.defaultdict(list)
    for path in sorted((out / "traces").glob("*.jsonl")):
        for e in read_jsonl(path):
            if e["ev"] == "tdom" and e["has_hll"]:
                classes[(path.stem, e["inv"])].append((e["filters"], e["hll"]))
    return classes


def formula_row(s, rels, classes, edges):
    """(coefficients of n per relation and d per class, log offset) of set s."""
    x = np.zeros(len(rels) + len(classes))
    for r in s:
        x[rels.index(r)] = 1

    parent = {r: r for r in s}

    def find(r):
        while parent[r] != r:
            r = parent[r]
        return r

    components, unused = len(s), set()
    for j in sorted(range(len(classes)), key=lambda j: -classes[j][1]):
        for f in classes[j][0]:
            members = edges.get(f)
            if not members or not set(members) <= set(s):
                continue
            if components == 1:
                unused.add(classes[j][1])
                continue
            a, b = find(members[0]), find(members[1])
            if a != b:
                parent[a] = b
                components -= 1
                x[len(rels) + j] -= 1
    return x, -math.log(1 + len(unused))


def main(out, source, name, weighting="flat"):
    out = pathlib.Path(out)
    classes = load_classes(out)
    edges = collections.defaultdict(dict)
    for e in read_jsonl(out / "edges.jsonl"):
        edges[(e["query"], e["inv"])][e["index"]] = e["set"]
    estimates = {(r["query"], r["inv"], tuple(r["set"])): r["est"] for r in read_jsonl(out / "estimates" / f"{source}.jsonl")}
    by_inv = collections.defaultdict(list)
    for q in read_jsonl(out / "questions.jsonl"):
        if q["status"] == "ok":
            by_inv[(q["query"], q["inv"])].append(q)

    rows, fidelity = [], []
    for key, qs in by_inv.items():
        rels = sorted({r for q in qs for r in q["set"]})
        cls = classes.get(key, [])
        built = [formula_row(q["set"], rels, cls, edges[key]) for q in qs]
        X = np.array([b[0] for b in built])
        offset = np.array([b[1] for b in built])

        # DuckDB's own inputs must give DuckDB's own estimates.
        duck = {q["set"][0]: q["est"] for q in qs if len(q["set"]) == 1}
        if len(duck) == len(rels) and all(v > 0 for v in duck.values()):
            theta = np.array([math.log(duck[r]) for r in rels] + [math.log(h) for _, h in cls])
            fidelity += [abs(x @ theta + o - math.log(q["est"])) for q, x, o in zip(qs, X, offset) if q["est"] > 0]

        y, w, keep = [], [], []
        for i, q in enumerate(qs):
            est = estimates.get((q["query"], q["inv"], tuple(q["set"])))
            if est and est > 0:
                y.append(math.log(est) - offset[i])
                w.append(len(q["set"]) if weighting == "size" else 1.0)
                keep.append(i)
        if not keep:
            continue
        sw = np.sqrt(np.array(w))
        theta, *_ = np.linalg.lstsq(X[keep] * sw[:, None], np.array(y) * sw, rcond=None)
        for q, x, o in zip(qs, X, offset):
            rows.append({"query": q["query"], "inv": q["inv"], "set": q["set"], "est": float(math.exp(x @ theta + o))})

    f = np.array(fidelity)
    print(f"DuckDB reproduced within 1% on {np.mean(f < 0.01):.1%} of {len(f)} sets", file=sys.stderr)
    write_jsonl(out / "estimates" / f"{name}.jsonl", rows)


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])
