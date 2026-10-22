#!/bin/sh
# Download the IMDB tables used by JOB (DuckDB's Parquet copy) into data/imdb.
set -eu
url=https://github.com/duckdb/duckdb-data/releases/download/v1.0
dir=data/imdb
mkdir -p $dir
for t in aka_name aka_title cast_info char_name comp_cast_type company_name company_type \
         complete_cast info_type keyword kind_type link_type movie_companies movie_info \
         movie_info_idx movie_keyword movie_link name person_info role_type title; do
    [ -f $dir/$t.parquet ] || curl -fsSL -o $dir/$t.parquet $url/job_$t.parquet
done
(cd $dir && shasum -a 256 *.parquet > SHA256SUMS)
