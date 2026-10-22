#!/bin/sh
# Build data/dsb.duckdb (DSB, scale factor 10) and its SPJ queries in data/workloads/dsb.
#
# DSB is TPC-DS with skewed and correlated data (github.com/microsoft/dsb). Its own generator
# makes the correlations: DuckDB's tpcds extension does not. Needs a C compiler and bison
# (macOS: brew install bison).
set -eu
root=$(pwd)
tools=vendor/dsb/code/tools
raw=data/dsb/raw
db=data/dsb.duckdb
out=data/workloads/dsb
duckdb=vendor/bin/duckdb-v2.0.0-baseline

# Templates of SPJ queries with equi-joins only. Excluded: 040, 072 (outer joins),
# 050, 100, 101, 102 (non-equi joins).
templates="013 018 019 025 027 084 085 091 099"
instances=25
seed=997

# 1. The generators. macOS lacks values.h and malloc.h: shims stand in.
shim=$(mktemp -d)
printf '#include <limits.h>\n#include <float.h>\n#ifndef MAXINT\n#define MAXINT INT_MAX\n#endif\n' > $shim/values.h
printf '#include <stdlib.h>\n' > $shim/malloc.h
yacc=$(command -v bison || echo /opt/homebrew/opt/bison/bin/bison)
(cd $tools && make -s OS=LINUX LINUX_CC=cc "LINUX_YACC=$yacc -y" \
    "LINUX_CFLAGS=-g -fcommon -I$shim -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-deprecated-non-prototype -Wno-return-type" \
    dsdgen dsqgen distcomp)

# 2. The data: all tables at once, as DSB requires for its correlations.
rm -rf $raw && mkdir -p $raw
(cd $tools && ./dsdgen -scale 10 -terminate N -force -dir $root/$raw)

# 3. The database, with DSB's schema and its primary keys.
rm -f $db
{
    cat vendor/dsb/scripts/create_tables.sql
    for f in $raw/*.dat; do
        echo "COPY $(basename $f .dat) FROM '$f' (DELIMITER '|', HEADER false, NULLSTR '');"
    done
} | $duckdb $db

# 4. The queries: DSB's sample workload parameters (normal distribution, sigma 2).
rm -rf $out && mkdir -p $out
tmp=$(mktemp -d)
(cd $tools && ./distcomp -i tpcds.dst -o tpcds.idx -param_dist normal -param_sigma 2 -param_center 0 -rngseed $seed > /dev/null)
for t in $templates; do
    (cd $tools && ./dsqgen -output_dir $tmp -streams $instances -directory ../../query_templates_pg/spj_queries \
        -template query${t}_spj.tpl -dialect postgres -param_dist normal -rngseed $seed > /dev/null)
    for i in $(seq 0 $((instances - 1))); do
        cp $tmp/query_$i.sql $out/q${t}_$(printf %02d $i).sql
    done
done

# Identical instances: keep one.
for f in $out/*.sql; do
    for g in $out/*.sql; do
        [ "$g" \< "$f" ] && cmp -s "$f" "$g" && rm "$f" && break
    done
done
