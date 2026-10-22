"""Plan quality under injected estimates (P-error).

    uv run plans.py <db> <out> <estimator> ...

For each query, the patched DuckDB plans with an estimator's cardinalities
injected ($DUCKDB_CE_INJECT). Each plan is then costed with true
cardinalities, by DuckDB's own cost model:

    cost(join) = card(join) + cost(left) + cost(right)      cost(leaf) = 0

    P-error = cost(plan from estimates) / cost(plan from true cardinalities)

Every plan pays the final result, which dilutes P-error when the result is
large. "inner" P-error leaves it out.

Estimators:

    duckdb          DuckDB's own estimates: nothing injected.
    duckdb_inject   DuckDB's estimates, injected: checks the injection.
    true            true cardinalities.
    bn_*, true_base base cardinalities only: DuckDB's join formula does the rest.
    fanout_<limit>  the fanout join estimator (joins.py).
    other           a file in <out>/estimates/.

Greedy enumeration (12+ relations) asks about different sets when the
estimates change, and the dataset only holds DuckDB's own questions. So
planning repeats until every question is answered: true counts the new
sets, fanout estimates them. Questions still left to DuckDB are "missing".
Writes <out>/plans/<estimator>.jsonl, the final injection of each query
in <out>/plans/<estimator>/<query>.txt (for runtime.py), and prints
P-error quantiles.
"""

import concurrent.futures
import json
import pathlib
import sys
import tempfile
import threading

import duckdb

import bn
import joins
from ceq import PATCHED, explain, fast_sql, plain_sql, question_parts, read_jsonl, run_count, write_jsonl

QUANTILES = (0.5, 0.9, 0.95, 0.99)

# Planning rounds per query, each answering the questions of the previous one.
MAX_ROUNDS = 10

# Estimators whose joins use DuckDB's formula: only their bases are injected.
BASE_ONLY = ("bn_", "true_base")


def key(r):
    return (r["query"], r["inv"], tuple(r["set"]))


class Injection:
    """What an estimator injects, and how it answers new questions."""

    def __init__(self, estimates, answers=lambda rels: True, extend=None):
        self.estimates = estimates  # {(query, inv, set): estimate}
        self.answers = answers      # set -> whether the estimator answers it
        self.extend = extend        # (query, inv, set) -> estimate or None
        self._lock = threading.Lock()

    def estimate(self, query, inv, rels):
        """Answer a new question, or None."""
        if self.extend is None:
            return None
        with self._lock:
            try:
                est = self.extend(query, inv, rels)
            except ValueError:
                # e.g. a set with a derived relation: no true count.
                est = None
            if est is not None:
                self.estimates[(query, inv, rels)] = est
            return est


def injection(name, db, out, questions, graphs, truth):
    if name == "duckdb":
        return Injection({}, answers=lambda rels: False)
    if name == "duckdb_inject":
        return Injection({key(q): q["est"] for q in questions})
    if name == "true":
        return Injection({key(q): q["true"] for q in questions if q["status"] == "ok"}, extend=truth)

    rows = read_jsonl(out / "estimates" / f"{name}.jsonl")
    if name.startswith(BASE_ONLY):
        single = lambda rels: len(rels) == 1
        return Injection({key(r): r["est"] for r in rows if single(r["set"])}, answers=single)
    estimates = {key(r): r["est"] for r in rows}
    if name.startswith("fanout_"):
        return Injection(estimates, extend=fanout(db, graphs, name.removeprefix("fanout_")))
    return Injection(estimates)


def fanout(db, graphs, limit):
    """(query, inv, set) -> fanout estimate or None, with models fitted as in bn.py."""
    con = duckdb.connect(str(db), read_only=True)
    schema = joins.infer_schema(con, graphs.values())
    estimator = joins.JoinEstimator(schema, bn.fit_tables(con, schema, bn.local_filters(graphs), limit))

    def answer(query, inv, rels):
        graph_rels, edges = graphs[(query, inv)]
        parts, status = question_parts({"filters": edges}, graph_rels, list(rels))
        if status != "ok":
            return None
        return estimator.estimate({r["id"]: r for r in graph_rels}, *parts)
    return answer


def write_injection(path, query, estimates):
    """The $DUCKDB_CE_INJECT file of one query."""
    with open(path, "w") as f:
        for (q, inv, rels), est in estimates.items():
            if q == query:
                f.write(f"{inv} [{','.join(map(str, rels))}] {est!r}\n")


def run(db, query, estimates):
    """One optimizer run -> trace events."""
    with tempfile.TemporaryDirectory() as tmp:
        inject = pathlib.Path(tmp) / "inject.txt"
        trace = pathlib.Path(tmp) / "trace.jsonl"
        write_injection(inject, query["query"], estimates)
        explain(PATCHED, db, query["sql"], trace=trace, inject=inject)
        return read_jsonl(trace)


def plan(db, query, inj):
    """Plan until every question is answered -> (events, missing, rounds)."""
    for rounds in range(1, MAX_ROUNDS + 1):
        events = run(db, query, inj.estimates)
        missing = {
            (e["inv"], tuple(e["set"])) for e in events
            if e["ev"] == "question" and not e["cache_hit"] and not e.get("injected") and inj.answers(e["set"])
        }
        if not missing:
            break
        answered = [inj.estimate(query["query"], inv, rels) for inv, rels in sorted(missing)]
        if all(a is None for a in answered):
            break
    return events, len(missing), rounds


def nodes(tree):
    """Join nodes of a plan tree: leaves are relation ids."""
    if isinstance(tree, int) or tree is None:
        return []
    return [tree] + nodes(tree["left"]) + nodes(tree["right"])


def cost(tree, card, inner=False):
    """Sum of join cardinalities. inner: without the final result."""
    joins = nodes(tree)[1:] if inner else nodes(tree)
    return sum(card(tuple(n["set"])) for n in joins)


def scorable(trees, query, truth):
    """Invocations whose joins all have true counts: e.g. not the ones that join a subquery's
    result, as in TPC-H. Every plan of an invocation joins its full set, so all plans agree."""
    result = {}
    for inv, tree in trees.items():
        try:
            if nodes(tree):
                truth(query, inv, tuple(nodes(tree)[0]["set"]))
        except ValueError:
            continue
        result[inv] = tree
    return result


class Truth:
    """True cardinalities: from the dataset, else counted and cached."""

    def __init__(self, db, out, questions, graphs):
        self._graphs = graphs
        self._known = {key(q): q["true"] for q in questions if q["status"] == "ok"}
        self._path = out / "truth_cache.jsonl"
        self._cache = {r["sql"]: r["true"] for r in read_jsonl(self._path)} if self._path.exists() else {}
        self._con = duckdb.connect(str(db), read_only=True)
        self._lock = threading.Lock()

    def __call__(self, query, inv, rels):
        k = (query, inv, rels)
        if k in self._known:
            return self._known[k]

        graph_rels, edges = self._graphs[(query, inv)]
        parts, status = question_parts({"filters": edges}, graph_rels, list(rels))
        if status != "ok":
            raise ValueError(f"{query} inv {inv} {rels}: {status}")
        sql = plain_sql(graph_rels, *parts)
        with self._lock:
            if sql not in self._cache:
                n, secs = run_count(self._con, fast_sql(graph_rels, *parts))
                self._cache[sql] = n
                with open(self._path, "a") as f:
                    f.write(json.dumps({"sql": sql, "true": n, "seconds": round(secs, 3)}) + "\n")
        self._known[k] = self._cache[sql]
        return self._known[k]


def evaluate(db, queries, graphs, truth, inj, best=None):
    """Plan every query. best: {query: cost of the true plan}."""
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        planned = list(pool.map(lambda q: plan(db, q, inj), queries))

    rows = []
    for q, (events, missing, rounds) in zip(queries, planned):
        # Injection keys on invocation ids: they must match the dataset's.
        for e in events:
            if e["ev"] != "invocation":
                continue
            expected = [r.get("table") for r in graphs.get((q["query"], e["inv"]), ([], []))[0]]
            if [r.get("table") for r in e["relations"]] != expected:
                raise RuntimeError(f"{q['query']} inv {e['inv']}: relations differ from the dataset")

        trees = {e["inv"]: e["tree"] for e in events if e["ev"] == "plan"}
        scored = scorable(trees, q["query"], truth)
        row = {"query": q["query"], "missing": missing, "rounds": rounds, "plans": scored,
               "skipped": len(trees) - len(scored)}
        trees = scored
        for field, inner in (("cost", False), ("inner", True)):
            row[field] = sum(cost(t, lambda s, inv=inv: truth(q["query"], inv, s), inner) for inv, t in trees.items())
        if best is not None and best[q["query"]]["cost"]:
            row["p_error"] = row["cost"] / best[q["query"]]["cost"]
        if best is not None and best[q["query"]]["inner"]:
            row["inner_p_error"] = row["inner"] / best[q["query"]]["inner"]
        rows.append(row)
    return rows


def summary(name, rows):
    errors = sorted(r["p_error"] for r in rows if "p_error" in r)
    inner = sorted(r["inner_p_error"] for r in rows if "inner_p_error" in r)
    pick = lambda xs, p: xs[min(len(xs) - 1, int(p * len(xs)))]
    total = sum(r["cost"] for r in rows if "p_error" in r)
    stats = " ".join(f"{pick(errors, p):>8.2f}" for p in QUANTILES)
    print(f"{name:<40} {len(errors):>4} {stats} {errors[-1]:>9.1f} {min(errors):>6.2f} "
          f"{sum(e > 1.0001 for e in errors):>5} {total:>11.3g} {sum(r['missing'] for r in rows):>7}"
          f" | inner {pick(inner, 0.5):.2f} {pick(inner, 0.95):.2f} {pick(inner, 0.99):.2f} {inner[-1]:.1f}")


def save(out, name, queries, rows, inj):
    write_jsonl(out / f"plans/{name}.jsonl", rows)
    (out / "plans" / name).mkdir(exist_ok=True)
    for q in queries:
        write_injection(out / "plans" / name / f"{q['query']}.txt", q["query"], inj.estimates)


def main(db, out, *names):
    out = pathlib.Path(out)
    queries = read_jsonl(out / "queries.jsonl")
    questions = read_jsonl(out / "questions.jsonl")
    graphs = bn.load_graphs(out)
    truth = Truth(db, out, questions, graphs)
    (out / "plans").mkdir(exist_ok=True)

    # The true plans first: they are every P-error's denominator.
    inj = injection("true", db, out, questions, graphs, truth)
    true_rows = evaluate(db, queries, graphs, truth, inj)
    best = {r["query"]: {"cost": r["cost"], "inner": r["inner"]} for r in true_rows}
    for r in true_rows:
        r["p_error"] = r["inner_p_error"] = 1.0
    save(out, "true", queries, true_rows, inj)

    print(f"{'estimator':<40} {'n':>4} {'p50':>8} {'p90':>8} {'p95':>8} {'p99':>8} {'max':>9} {'min':>6} {'worse':>5} {'total cost':>11} {'missing':>7}")
    summary("true", true_rows)
    for name in names:
        inj = injection(name, db, out, questions, graphs, truth)
        rows = evaluate(db, queries, graphs, truth, inj, best)
        save(out, name, queries, rows, inj)
        summary(name, rows)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
