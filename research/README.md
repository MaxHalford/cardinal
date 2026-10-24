# Research

How good are DuckDB's cardinality estimates, and how much better do Bayesian networks ([sorobn](https://github.com/MaxHalford/sorobn)) make them? Run everything from this directory.

- `patches/`: a DuckDB patch that logs every cardinality question the join-order optimizer asks, and can inject estimates instead.
- `ceq.py` turns those logs into a dataset of questions with true counts.
- `bn.py` answers the questions with per-table Bayesian networks (`tables.py`) and fanout columns for joins (`joins.py`). `score.py` compares q-errors.
- `plans.py` injects estimates into DuckDB and scores the chosen plans with true cardinalities (P-error). `runtime.py` times them.
- `ext.py` scores the plans the `cardinal` extension leads DuckDB to. `qerror.py` compares the estimates DuckDB plans with, `worse.py` lists plans worse than DuckDB's own.
- `keyjoin.py` compares assumptions for key joins, with exact per-table counts: today's fanout, and groups of keys.

## Setup

```sh
git submodule update --init
(cd ../duckdb && make release)
mkdir -p vendor/bin && cp ../duckdb/build/release/duckdb vendor/bin/duckdb-v2.0.0-baseline
(cd ../duckdb && git apply ../research/patches/duckdb-v2.0.0-ce-trace.patch && make release)
(cd .. && make release)   # the extension, on the patched DuckDB
```

## Benchmarks

| Name | Database | Workload |
|---|---|---|
| JOB-light | `./scripts/fetch_imdb.sh && ./scripts/load_imdb.sh` | `vendor/learnedcardinalities/workloads/job-light.sql` |
| JOB | same as JOB-light | `./scripts/job_queries.sh`, then `data/workloads/job` |
| STATS-CEB | `./scripts/load_stats.sh` | `vendor/cardest-benchmark/workloads/stats_CEB/stats_CEB.sql` |
| TPC-H (no harm check) | `./scripts/load_tpch.sh` | `data/workloads/tpch` |
| DSB | `./scripts/load_dsb.sh` | `data/workloads/dsb` |

```sh
w=vendor/learnedcardinalities/workloads/job-light.sql db=data/imdb.duckdb out=data/out/job-light
uv run ceq.py trace $w $db $out
uv run ceq.py build $out
uv run ceq.py truth $db $out
uv run ceq.py check $db $out
uv run plans.py $db $out duckdb true

# The extension, against DuckDB alone.
uv run ext.py warmup $db $out
uv run ext.py plans $db $out off fanout
uv run qerror.py $out
uv run worse.py $out
uv run runtime.py $db $out cardinal_off cardinal_fanout
```

Tests: `uv run pytest`.
