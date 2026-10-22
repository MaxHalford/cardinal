#!/bin/sh
# Build data/imdb.duckdb from data/imdb/*.parquet with stock DuckDB v2.0.0.
set -eu
db=data/imdb.duckdb
rm -f $db
for f in data/imdb/*.parquet; do
    t=$(basename $f .parquet)
    echo "CREATE TABLE $t AS SELECT * FROM read_parquet('$f');"
done | vendor/bin/duckdb-v2.0.0-baseline $db
