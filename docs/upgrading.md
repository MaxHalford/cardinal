# Upgrading DuckDB

The extension builds against one DuckDB release. It uses DuckDB internals: the optimizer's plan, scan callbacks, statistics and join-order classes. These change between releases without notice. Each release needs a port and the checks below.

## 1. Move the submodules

```sh
git -C duckdb fetch --depth 1 origin tag v2.0.0
git -C duckdb checkout v2.0.0
git -C extension-ci-tools fetch origin v2.0-cyanoptera   # the branch for that release
git -C extension-ci-tools checkout FETCH_HEAD
```

Set the Python package to the same version in `pyproject.toml` (`duckdb==2.0.0`), then `uv lock && uv sync`.

## 2. Build and port

```sh
rm -rf build && make release
```

Fix compile errors. Then read what changed in the parts the extension depends on:

| DuckDB part | Extension code | What to check |
|---|---|---|
| `LogicalGet` callbacks `pushdown_complex_filter`, `cardinality`, `statistics_extended` | `scan_hook.cpp` | still called, and in the same order: pushdown, then join order |
| `RelationStatisticsHelper::ExtractGetStats` | `scan_hook.cpp` `FilterFactor` | where the base row count comes from, and how filters scale it |
| `CardinalityEstimator` | `join_planner.cpp` `FormulaRow` | the join formula the fit reproduces: spanning edges, unused-edge penalty, caps |
| `JoinOrderOptimizer` | `join_planner.cpp` `DuckDBJoins` | it still runs on a plan copy |
| table filters, bound expressions | `bn_estimator.cpp`, `exact.cpp`, `query_graph.cpp` | filter classes, comparison and cast shapes |

## 3. Check

```sh
./build/release/test/unittest "test/*"   # the extension's tests
uv run scripts/gate.py all               # DuckDB's SQL tests, then the stress test
```

`gate.py suite` runs DuckDB's SQL tests with the extension. A test that fails with the extension and passes without it is a regression. `gate.py stress` runs concurrent queries, inserts and fits, and compares results with and without the extension. Both must pass.

## 4. Never worse than DuckDB

The research patch traces DuckDB's cardinality questions. Port it to the new release, as `patches/duckdb-<version>-ce-trace.patch`, then on each benchmark:

```sh
uv run ext.py warmup <db> <out>
uv run ext.py plans <db> <out> off fanout
```

Compare `cardinal_fanout` with `cardinal_off` in `<out>/plans/`. No query may get a plan with a higher cost.
