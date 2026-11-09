#!/bin/sh
# Build SSB (scale factor 10) and install its 13 queries.
set -eu

scale=10
revision=ae1e254aa4d603d8ef1f44078e5abed011634b23
archive_url="https://github.com/eyalroz/ssb-dbgen/archive/$revision.tar.gz"
root=$(pwd)
source_dir=data/ssb/dbgen
build="$source_dir/build"
raw=data/ssb/raw
db=data/ssb.duckdb
out=data/workloads/ssb
duckdb=vendor/bin/duckdb-v2.0.0-baseline

if [ ! -f "$source_dir/CMakeLists.txt" ]; then
    mkdir -p "$source_dir"
    curl -L "$archive_url" | tar -xz --strip-components=1 -C "$source_dir"
fi

cmake -S "$source_dir" -B "$build" -DCMAKE_BUILD_TYPE=Release -DEOL_HANDLING=ON
cmake --build "$build" -j 8

rm -rf "$raw"
mkdir -p "$raw"
(cd "$raw" && "$root/$build/dbgen" -b "$root/$source_dir/dists.dss" -s "$scale")

rm -f "$db"
{
    cat workloads/ssb/schema.sql
    for table in part supplier customer date lineorder; do
        echo "COPY \"$table\" FROM '$root/$raw/$table.tbl' (DELIMITER '|', HEADER false);"
    done
} | "$duckdb" -no-agent "$db"

rm -rf "$out"
mkdir -p "$out"
cp workloads/ssb/q*.sql "$out"
