# cardinal for PostgreSQL

A spike: PostgreSQL's planner with join sizes set from outside. PostgreSQL has no hook for join sizes, so the extension replaces the join search (`join_search_hook`) with PostgreSQL's own, copied in `joinrels.c` from PostgreSQL 18.6, which sets each join's rows before costing its paths, as [pg_hint_plan](https://github.com/ossc-db/pg_hint_plan) does.

For now, sizes come from a setting, by table alias: `SET cardinal.rows = 'a,b=1000000; b,c=10';`. Next: cardinal's estimates.

```sh
make PG_CONFIG=/path/to/pg_config
psql -d mydb -v library="$PWD/cardinal.dylib" -f test/spike.sql
```

Each PostgreSQL major version needs its own copy of `joinrels.c`.
