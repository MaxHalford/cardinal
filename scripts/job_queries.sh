#!/bin/sh
# Copy the original JOB queries to data/workloads/job.
# "at" is reserved in DuckDB: rename that alias to "at_", as DuckDB's own copy does.
set -eu
dir=data/workloads/job
rm -rf $dir && mkdir -p $dir
for f in vendor/join-order-benchmark/[0-9]*.sql; do
    perl -pe 's/\bat([.,])/at_$1/g' $f > $dir/$(basename $f)
done
