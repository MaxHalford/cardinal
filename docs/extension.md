# The cardinal extension

DuckDB picks join orders from estimates of how many rows each step produces. Cardinal gives it better estimates, learned from your data, so it can avoid plans that build huge intermediate results.

```sql
LOAD cardinal;
```

That's it: queries are planned as before, with better numbers.

## What it does

When DuckDB plans a query, cardinal:

1. **Learns** from it: the columns its filters read, and the joins between tables. A join on a unique column (declared `PRIMARY KEY` or `UNIQUE`, or checked once) tells which column is a primary key and which a foreign key.
2. **Fits a model** per table it needs, if it has none yet: a Bayesian network over the columns filters read, plus, for each foreign key that points to the table, how many rows point to each of its rows. Fitting reads a sample of at most one million rows. It takes milliseconds for small tables, around a second for large ones, and happens once: the query that needs the model waits for it.
3. **Estimates** the query's filters and joins with the models, and gives DuckDB the row and distinct counts that make DuckDB's own formulas reproduce those estimates. DuckDB then plans as usual.

Cardinal never makes a plan worse by its own reckoning: it compares the plan DuckDB picks with its numbers and without them, and keeps its numbers only if their plan looks at least 5% cheaper. Otherwise, and for anything it can't estimate, the query is planned by DuckDB alone. Query results never change, only plans; if anything goes wrong inside cardinal, DuckDB plans without it.

## When models change

A model is refitted when its table's row count changes by more than 10%, or when a query filters on a column, or joins on a key, the model doesn't cover yet.

## Where models live

Models are saved in their own DuckDB file, never in your database:

```
~/.duckdb/extension_data/cardinal/cardinal.db
```

They're keyed by the path of your database file and the table, so a later session starts with them. Databases without a file, e.g. in memory, keep models in memory only. If another process is using the file, cardinal waits briefly, then keeps models in memory. Deleting the file forgets everything.

```sql
SELECT * FROM cardinal_models();   -- the models saved for the current database
```

## Functions and settings

| | Default | |
|---|---|---|
| `CALL cardinal_fit_eager(['table'])` | | fit now, rather than when queries need it; all tables without an argument |
| `SELECT * FROM cardinal_models()` | | the models saved for the current database |
| `SET cardinal_estimator` | `'fanout'` | `'off'` turns cardinal off; `'bn'` (filtered scans only) and `'exact'` (true counts) are for experiments |
| `SET cardinal_save` | `true` | save fitted models |
| `SET cardinal_store` | `''` | the models' file; empty: `~/.duckdb/extension_data/cardinal/cardinal.db` |
| `SET cardinal_background` | `false` | fit on a worker thread, planning with what there is meanwhile. The worker's transactions make `CHECKPOINT` fail while it runs, and keep a closed database open until the fit ends. |

## Limits

- The first query that needs a table's model waits for its fit.
- Estimates cover joins on equal columns that form a tree, each through a primary key. Other joins (cycles, joins between two non-unique columns without a known key) are left to DuckDB.
- `EXPLAIN` shows the row counts cardinal gives DuckDB, which are fitted to reproduce join estimates: they can differ from a table's real filtered row count.
- Built against DuckDB v1.5.6, and relies on DuckDB internals: each DuckDB release needs a new build.
