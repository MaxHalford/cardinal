#!/bin/sh
# Build data/tpch.duckdb (TPC-H, scale factor 10) and its 22 queries in data/workloads/tpch,
# with stock DuckDB v2.0.0 and its tpch extension.
set -eu
db=data/tpch.duckdb
out=data/workloads/tpch
rm -f $db
mkdir -p $out
duckdb=vendor/bin/duckdb-v2.0.0-baseline
$duckdb $db -c "INSTALL tpch; LOAD tpch; CALL dbgen(sf = 10);"
# One file per query, e.g. q01.sql.
$duckdb -noheader -list -c "INSTALL tpch; LOAD tpch; SELECT query_nr, query FROM tpch_queries();" -no-agent |
    uv run python -c "
import sys, re
text = sys.stdin.read()
for nr, sql in re.findall(r'^(\d+)\|(.*?)(?=^\d+\||\Z)', text, re.S | re.M):
    open(f'$out/q{int(nr):02d}.sql', 'w').write(sql.strip() + '\n')
"
