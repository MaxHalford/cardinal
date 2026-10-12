<div align="center">
    <h1>cardinal-duck — Bayesian networks for DuckDB's cardinality estimation</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB estimates how many rows each intermediate join produces, and picks a join order from those estimates. This project measures how good those estimates are, and replaces them with Bayesian networks ([sorobn](https://github.com/MaxHalford/sorobn)).

It has four parts:

- A patch that logs every cardinality question DuckDB's join-order optimizer asks, and can inject estimates instead.
- `ceq.py`, which turns those logs into a dataset of questions with true counts.
- `bn.py`, which answers the questions with per-table Bayesian networks (`tables.py`) and fanout columns for joins (`joins.py`). `score.py` compares q-errors.
- `plans.py`, which injects estimates into DuckDB and scores the chosen plans with true cardinalities (P-error).

Notes are in `notes/`.

<br clear="right"/>

## Setup

```sh
git submodule update --init
(cd vendor/duckdb && make release)
mkdir -p vendor/bin && cp vendor/duckdb/build/release/duckdb vendor/bin/duckdb-v1.5.6-baseline
(cd vendor/duckdb && git apply ../../patches/duckdb-v1.5.6-ce-trace.patch && make release)
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
