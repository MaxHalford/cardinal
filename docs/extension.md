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

## On by default, or opt in

Cardinal estimates every query once loaded. To use it only where you choose, turn it off for all connections, then on where needed:

```sql
SET GLOBAL cardinal_estimator = 'off';

SET cardinal_estimator = 'fanout';   -- this connection
SELECT ...;
SET cardinal_estimator = 'off';      -- not RESET: it restores the default, 'fanout'
```

While off, cardinal learns nothing: the first query it estimates learns the tables it reads and fits their models, and waits for the fit. To fit ahead of time, run the workload once with cardinal on, then fit:

```sql
CALL cardinal_fit_eager();          -- every table
CALL cardinal_fit_eager('title');   -- one table
```

Models are saved, so this is needed once per database and DuckDB release.

## When models change

A model is refitted when its table's row count changes by more than 10%, or when a query filters on a column, or joins on a key, the model doesn't cover yet.

## Where models live

Models are saved in their own DuckDB file, never in your database:

```
~/.duckdb/extension_data/cardinal/v2.0.0/store-1.db
```

Each DuckDB release has its own file, as extensions do: after an upgrade, cardinal learns and fits again from scratch, and old files can be deleted. They're keyed by the path of your database file and the table, so a later session starts with them. A moved or copied database file starts from scratch too. Databases without a file, e.g. in memory, keep models in memory only. If another process is using the file, cardinal waits briefly, then keeps models in memory. Deleting the file forgets everything.

```sql
SELECT * FROM cardinal_models();   -- the models saved for the current database
```

## Functions and settings

| | Default | |
|---|---|---|
| `CALL cardinal_fit_eager()`, `cardinal_fit_eager('table')` | | fit now, rather than when queries need it; all tables without an argument |
| `SELECT * FROM cardinal_models()` | | the models saved for the current database |
| `SET cardinal_estimator` | `'fanout'` | `'off'` turns cardinal off; `'bn'` (filtered scans only) and `'exact'` (true counts) are for experiments |
| `SET cardinal_save` | `true` | save fitted models |
| `SET cardinal_store` | `''` | the models' file, used as is; empty: the release's file above |
| `SET cardinal_background` | `false` | fit on a worker thread, planning with what there is meanwhile. The worker's transactions make `CHECKPOINT` fail while it runs, and keep a closed database open until the fit ends. |

## Limits

- The first query that needs a table's model waits for its fit.
- Estimates cover joins on equal columns that form a tree, each through a primary key. Other joins (cycles, joins between two non-unique columns without a known key) are left to DuckDB.
- `EXPLAIN` shows the row counts cardinal gives DuckDB, which are fitted to reproduce join estimates: they can differ from a table's real filtered row count.
- Built against DuckDB v2.0.0, and relies on DuckDB internals: each DuckDB release needs a port. See [upgrading.md](upgrading.md).
