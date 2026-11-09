#!/bin/sh
# Build DuckDB's LDBC SNB BI database (scale factor 0.1) and workload.
set -eu

archive_url=https://github.com/duckdb/duckdb-data/releases/download/v1.0/ldbc-snb-sf0.1.tar.gz
raw=data/ldbc/sf0.1
db=data/ldbc.duckdb
out=data/workloads/ldbc
duckdb=vendor/bin/duckdb-v2.0.0-baseline
benchmark=../duckdb/benchmark/ldbc

if [ ! -d "$raw" ]; then
    mkdir -p data/ldbc
    curl -L "$archive_url" | tar -xz -C data/ldbc
fi

rm -f "$db"
{
    cat "$benchmark/schema.sql"
    sed "s|PATHVAR|$(pwd)/$raw|" "$benchmark/snb-load.sql"
} | "$duckdb" -no-agent "$db"

rm -rf "$out"
mkdir -p "$out"
for query in "$benchmark"/queries/bi-*.sql; do
    # Q11 enables profiling before its query, which cannot follow EXPLAIN.
    sed '/^pragma enable_profiling;$/d' "$query" > "$out/$(basename "$query")"
done
