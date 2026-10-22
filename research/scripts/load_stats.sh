#!/bin/sh
# Build data/stats.duckdb (STATS-CEB) with stock DuckDB v2.0.0.
set -eu
db=data/stats.duckdb
dir=vendor/cardest-benchmark/datasets/stats_simplified
rm -f $db
{
    sed 's/SERIAL PRIMARY KEY/INTEGER/' $dir/stats.sql
    for t in badges comments postHistory postLinks posts tags users votes; do
        echo "COPY $t FROM '$dir/$t.csv' (HEADER);"
    done
} | vendor/bin/duckdb-v2.0.0-baseline $db
