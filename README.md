<div align="center">
    <h1>cardinal-duck — Bayesian networks for DuckDB's cardinality estimation</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB estimates how many rows each intermediate join produces, and picks a join order from those estimates. This project measures how good those estimates are, and replaces them with Bayesian networks ([sorobn](https://github.com/MaxHalford/sorobn)).

It has five parts:

- A patch that logs every cardinality question DuckDB's join-order optimizer asks, and can inject estimates instead.
- `ceq.py`, which turns those logs into a dataset of questions with true counts.
- `bn.py`, which answers the questions with per-table Bayesian networks (`tables.py`) and fanout columns for joins (`joins.py`). `score.py` compares q-errors.
- `plans.py`, which injects estimates into DuckDB and scores the chosen plans with true cardinalities (P-error).
- `cardinal`, a DuckDB extension (`src/`) that gives the optimizer Bayesian-network estimates of base tables, without patching DuckDB.

Notes are in `notes/`.

<br clear="right"/>

## Setup

The research tools need DuckDB patched to trace and inject estimates. The extension does not: it builds and runs on stock DuckDB.

```sh
git submodule update --init
(cd duckdb && make release)
mkdir -p vendor/bin && cp duckdb/build/release/duckdb vendor/bin/duckdb-v1.5.6-baseline
(cd duckdb && git apply ../patches/duckdb-v1.5.6-ce-trace.patch && make release)
```

## Benchmarks

| Name | Database | Workload |
|---|---|---|
| JOB-light | `./scripts/fetch_imdb.sh && ./scripts/load_imdb.sh` | `vendor/learnedcardinalities/workloads/job-light.sql` |
| JOB | same as JOB-light | `./scripts/job_queries.sh`, then `data/workloads/job` |
| STATS-CEB | `./scripts/load_stats.sh` | `vendor/cardest-benchmark/workloads/stats_CEB/stats_CEB.sql` |

```sh
w=vendor/learnedcardinalities/workloads/job-light.sql db=data/imdb.duckdb out=data/out/job-light
uv run ceq.py trace $w $db $out
uv run ceq.py build $out
uv run ceq.py truth $db $out
uv run ceq.py check $db $out
uv run bn.py $db $out max_categories=500,coverage=0.9
uv run score.py $out bn_max_categories=500,coverage=0.9 fanout_max_categories=500,coverage=0.9
uv run plans.py $db $out duckdb bn_max_categories=500,coverage=0.9 fanout_max_categories=500,coverage=0.9
```

Tests: `uv run python -m pytest`.

## Extension

```sh
make release      # DuckDB with the extension: build/release/duckdb
```

```sql
LOAD cardinal;    -- that's it
```

The extension learns from the queries it plans: the columns they filter on, and the primary and foreign keys they join. It fits a model per table on those, with a fanout column per foreign key, on a worker thread, and refits when a table's row count drifts by 10%. Models and keys are saved in the `__cardinal` schema of the database, unless it is read-only, in memory, or not a DuckDB database; `DROP SCHEMA __cardinal CASCADE` forgets them.

DuckDB estimates joins with its own formula, from row counts and distinct counts. Per query, the extension fits those numbers to its join estimates, so DuckDB's optimizer plans with them.

- `CALL cardinal_fit_eager(['table'])`: fit now, rather than when queries need it.
- `SET cardinal_estimator = 'off'`: turn it off. `bn` (filtered scans only) and `exact` (true counts) are for experiments.
- `SET cardinal_background = false`: queries wait for fits, for deterministic tests and benchmarks.

`export.py` writes models fitted by `bn.py` instead. Tests: `make test`.
