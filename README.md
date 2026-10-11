# cardinal-duck

Bayesian-network cardinality estimation for DuckDB.

## Setup

```sh
git submodule update --init
(cd vendor/duckdb && make release)
mkdir -p vendor/bin && cp vendor/duckdb/build/release/duckdb vendor/bin/duckdb-v1.5.6-baseline
(cd vendor/duckdb && git apply ../../patches/duckdb-v1.5.6-ce-trace.patch && make release)
./scripts/fetch_imdb.sh && ./scripts/load_imdb.sh
```

## Question dataset

```sh
w=vendor/learnedcardinalities/workloads/job-light.sql
uv run ceq.py trace $w data/imdb.duckdb data/out/job-light
uv run ceq.py build data/out/job-light
uv run ceq.py truth data/imdb.duckdb data/out/job-light
uv run ceq.py check data/imdb.duckdb data/out/job-light
```

Notes are in `notes/`.
