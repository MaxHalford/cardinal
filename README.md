<div align="center">
    <h1>cardinal-duck — Bayesian networks for DuckDB's cardinality estimation</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB estimates how many rows each intermediate join produces, and picks a join order from those estimates. This project measures how good those estimates are, and replaces them with Bayesian networks ([sorobn](https://github.com/MaxHalford/sorobn)).

It has three parts:

- A patch that logs every cardinality question DuckDB's join-order optimizer asks.
- `ceq.py`, which turns those logs into a dataset of questions with true counts.
- `bn.py` and `score.py`, which answer the questions with Bayesian networks and compare q-errors.

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
uv run bn.py $db $out
uv run score.py $out bn_table true_base
```
