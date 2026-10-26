# /// script
# requires-python = ">=3.13"
# dependencies = ["duckdb==2.0.0"]
# ///
"""Upgrade gate: checks the extension against the DuckDB it is built with.

    uv run scripts/gate.py suite     DuckDB's SQL tests: none may fail only with the extension
    uv run scripts/gate.py stress    concurrent queries, inserts and fits: no crash, same results
    uv run scripts/gate.py all

Needs `make release`. Writes reports to build/gate/. Exits 1 on failure.

suite runs DuckDB's test/sql in chunks, one process each, with the extension
estimating ('on'), on disk too, and estimating nothing ('off'). Tests
that fail with 'on' are re-run alone against 'off': a test that fails
only with the extension is a regression. Some tests fail with 'off' too,
e.g. when they need an extension the build lacks: they are not ours.
"""

import concurrent.futures
import json
import pathlib
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build/gate"
UNITTEST = ROOT / "build/release/test/unittest"
EXTENSION = ROOT / "build/release/extension/cardinal/cardinal.duckdb_extension"

# Tests per process: a crash loses one chunk, not the run.
CHUNK = 100
WORKERS = 4
CHUNK_TIMEOUT = 1800
TEST_TIMEOUT = 900

# Estimators compared by the suite: the extension's default, and none.
ON = "on"
OFF = "off"


def config(estimator):
    """unittest config: the extension loaded, its store in the gate dir."""
    path = OUT / f"config-{estimator}.json"
    store = OUT / "suite_store.db"
    setting = f"SET cardinal_store = '{store}'; SET cardinal_estimator = '{estimator}'"
    path.write_text(json.dumps({
        "statically_loaded_extensions": ["core_functions", "cardinal"],
        "on_new_connection": setting,
    }))
    return path


def unittest(args, timeout):
    """unittest's output and exit code, in a temp dir of its own."""
    temp = OUT / f"tmp-{uuid.uuid4().hex}"
    cmd = [str(UNITTEST), "--test-dir", str(ROOT / "duckdb"), "--test-temp-dir", str(temp), *args]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out, code = result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        code = "timeout"
    shutil.rmtree(temp, ignore_errors=True)
    return out, code


def chunk(args, start, count):
    """Failed tests of one chunk, and the test that crashed it, if any."""
    out, code = unittest([*args, "--start-offset", str(start), "--end-offset", str(min(start + CHUNK, count)),
                          "test/sql/*"], CHUNK_TIMEOUT)
    failed = set(re.findall(r"^(test/\S+\.test):\d+: FAILED", out, re.M))

    # A crash ends the process: the last test started is the culprit.
    started = re.findall(r"\[\d+/\d+\] \(\d+%\): (test/\S+)", out)
    finished = "test cases:" in out or "All tests passed" in out
    if code == "timeout":
        return failed, f"chunk {start}: timeout"
    if not finished and started:
        return failed, started[-1]
    # No summary and no test started: unittest itself failed, e.g. a bad config.
    if not finished:
        return failed, f"chunk {start}: no result"
    return failed, None


def run_suite(args):
    """Failed and crashed tests of test/sql, in parallel chunks."""
    tests = subprocess.run([str(UNITTEST), "--test-dir", str(ROOT / "duckdb"), "--list-test-names-only", "test/sql/*"],
                           capture_output=True, text=True).stdout.split()
    with concurrent.futures.ThreadPoolExecutor(WORKERS) as pool:
        results = list(pool.map(lambda start: chunk(args, start, len(tests)), range(0, len(tests), CHUNK)))
    failed = set().union(*(f for f, _ in results))
    crashed = {c for _, c in results if c}
    return len(tests), failed, crashed


def fails_alone(test, estimator, storage):
    out, code = unittest(["--test-config", str(config(estimator)), *(["--force-storage"] if storage else []), test],
                         TEST_TIMEOUT)
    return not ("All tests passed" in out or ("test cases:" not in out and code == 0))


def suite():
    """Regressions: tests that fail with the extension estimating, and not without."""
    report = {}
    for name, args in ((ON, ["--test-config", str(config(ON))]),
                       ("storage", ["--test-config", str(config(ON)), "--force-storage"])):
        count, failed, crashed = run_suite(args)
        report[name] = {"tests": count, "failed": sorted(failed), "crashed": sorted(crashed)}
        print(f"{name}: {count} tests, {len(failed)} failed, {len(crashed)} crashed", flush=True)

    # Candidates, alone: concurrency and test order no longer play.
    regressions = []
    for name, storage in ((ON, False), ("storage", True)):
        for test in report[name]["failed"] + report[name]["crashed"]:
            if not test.startswith("test/"):
                regressions.append(test)
                continue
            if fails_alone(test, ON, storage) and not fails_alone(test, OFF, storage):
                regressions.append(f"{'disk ' if storage else ''}{test}")
                print(f"regression: {regressions[-1]}", flush=True)
    report["regressions"] = regressions
    (OUT / "suite.json").write_text(json.dumps(report, indent=1))
    print(f"suite: {len(regressions)} regressions")
    return not regressions


# Stress: readers compare results with and without the extension, while writers grow
# tables, replace them and fit models. Each mode runs in its own process: a crash shows
# as its exit code.
STRESS_SECONDS = 20
STRESS_ROUNDS = 3
STRESS_READERS = 6
STRESS_WRITERS = 2

STRESS_TABLES = """
CREATE OR REPLACE TABLE dim AS SELECT range AS id, range % 7 AS color, 'n' || (range % 97) AS name FROM range(5000);
CREATE OR REPLACE TABLE big AS SELECT range AS id, range % 1000 AS year, (range * 7919) % 5000 AS dim_id FROM range(2000000);
CREATE OR REPLACE TABLE fact AS SELECT range % 2000000 AS big_id, (range * 31) % 5000 AS dim_id, range % 13 AS kind FROM range(3000000);
CREATE OR REPLACE TABLE growing AS SELECT range AS id, range % 5 AS v FROM range(10000);
"""

STRESS_QUERIES = [
    "SELECT count(*) FROM big b, fact f WHERE b.id = f.big_id AND b.year < {a} AND f.kind = {k}",
    "SELECT count(*) FROM dim d, fact f, big b WHERE d.id = f.dim_id AND b.id = f.big_id AND d.color = {c} AND b.year > {a}",
    "SELECT count(*) FROM dim d, big b WHERE d.id = b.dim_id AND d.name LIKE 'n{c}%'",
    "SELECT count(*) FROM fact f, big b, dim d WHERE f.big_id = b.id AND b.dim_id = d.id AND f.dim_id = d.id AND f.kind IN ({k}, {c})",
    "SELECT d.color, count(*) FROM dim d LEFT JOIN fact f ON d.id = f.dim_id WHERE d.color < {c} GROUP BY ALL ORDER BY ALL",
    "SELECT count(*) FROM growing g, dim d WHERE g.id = d.id AND g.v = {c}",
    "SELECT count(*) FROM (SELECT big_id FROM fact WHERE kind = {k}) s JOIN big b ON s.big_id = b.id WHERE b.year BETWEEN {a} AND {a} + 50",
]

# Background fitting: off by default, on as an option.
BACKGROUND_MODES = ("false", "true")


def stress_process(path, background):
    """One stress run, in this process. Exits 1 on wrong results or extension errors."""
    import duckdb

    def connect():
        con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
        con.execute(f"LOAD '{EXTENSION}'")
        con.execute(f"SET GLOBAL cardinal_store = '{path}.store'")
        con.execute(f"SET GLOBAL cardinal_background = {background}")
        return con

    con = connect()
    con.execute(STRESS_TABLES)
    con.close()
    errors = []

    def reader(con, stop):
        cur = con.cursor()
        rng = random.Random()
        while not stop.is_set():
            sql = rng.choice(STRESS_QUERIES).format(a=rng.randint(0, 999), k=rng.randint(0, 12), c=rng.randint(0, 6))
            # growing changes between the two runs: its results may differ.
            racy = "growing" in sql
            try:
                cur.execute(f"SET cardinal_estimator = '{ON}'")
                got = cur.execute(sql).fetchall()
                cur.execute(f"SET cardinal_estimator = '{OFF}'")
                want = cur.execute(sql).fetchall()
                if not racy and got != want:
                    errors.append(f"wrong result: {sql}: {got} vs {want}")
            except Exception as e:
                if not racy and "does not exist" not in str(e):
                    errors.append(f"{type(e).__name__}: {e} in {sql}")

    def writer(con, stop):
        cur = con.cursor()
        rng = random.Random()
        while not stop.is_set():
            action = rng.random()
            try:
                if action < 0.5:
                    cur.execute(f"INSERT INTO growing SELECT range, range % 5 FROM range({rng.randint(1000, 5000)})")
                elif action < 0.7:
                    cur.execute("CREATE OR REPLACE TABLE growing AS SELECT range AS id, range % 5 AS v FROM range(10000)")
                elif action < 0.85:
                    cur.execute("SELECT count(*) FROM cardinal_fit_eager('dim')").fetchall()
                else:
                    cur.execute("SELECT count(*) FROM cardinal_fit_eager()").fetchall()
            except Exception as e:
                # The writers' own DDL conflicts: expected, e.g. "Failed to commit" in v2. Anything
                # from the extension: not.
                expected = any(text in str(e) for text in ("onflict", "does not exist", "Failed to commit"))
                if "cardinal" in str(e) or not expected:
                    errors.append(f"writer {type(e).__name__}: {e}")
            time.sleep(rng.random() * 0.2)

    for round_ in range(STRESS_ROUNDS):
        con = connect()
        stop = threading.Event()
        threads = [threading.Thread(target=reader, args=(con, stop)) for _ in range(STRESS_READERS)]
        threads += [threading.Thread(target=writer, args=(con, stop)) for _ in range(STRESS_WRITERS)]
        for t in threads:
            t.start()
        time.sleep(STRESS_SECONDS)
        stop.set()
        for t in threads:
            t.join()
        # Close while background fits may still run.
        con.close()
        print(f"round {round_}: {len(errors)} errors so far", flush=True)

    for e in errors[:20]:
        print(e)
    sys.exit(1 if errors else 0)


def stress():
    ok = True
    for background in BACKGROUND_MODES:
        path = OUT / f"stress-{background}.duckdb"
        for stale in (path, pathlib.Path(f"{path}.store"), pathlib.Path(f"{path}.wal")):
            stale.unlink(missing_ok=True)
        result = subprocess.run([sys.executable, __file__, "stress-process", str(path), background])
        print(f"stress, background {background}: exit {result.returncode}", flush=True)
        ok = ok and result.returncode == 0
    return ok


if __name__ == "__main__":
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "stress-process":
        stress_process(*sys.argv[2:4])
    OUT.mkdir(parents=True, exist_ok=True)
    checks = {"suite": [suite], "stress": [stress], "all": [suite, stress]}
    if command not in checks:
        sys.exit(__doc__)
    results = [check() for check in checks[command]]
    sys.exit(0 if all(results) else 1)
