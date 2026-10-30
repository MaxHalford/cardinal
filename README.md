<div align="center">
    <h1>cardinal — better cardinality estimates for DuckDB</h1>
    <a href="https://github.com/MaxHalford/cardinal/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/MaxHalford/cardinal/actions/workflows/MainDistributionPipeline.yml/badge.svg" alt="CI"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"></a>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB picks a join order from estimates of how many rows each join produces. Those estimates assume columns are independent, so on real data they can be off by orders of magnitude, and DuckDB picks slow plans.

`cardinal` is a DuckDB extension that learns Bayesian networks from your tables, and gives DuckDB their estimates instead. It needs no configuration: load it, and queries are planned with better numbers. Results never change, only plans. On the STATS-CEB benchmark, total runtime drops from 130 s to 67 s.

<br clear="right"/>

## Quick start

Cardinal isn't in DuckDB's [community extensions](https://duckdb.org/community_extensions/) yet, so build it from source. You need what [DuckDB's build](https://duckdb.org/docs/stable/dev/building/overview) needs: Git, CMake and a C++ compiler. The build compiles DuckDB v2.0.0 itself: about 15 minutes on an Apple M5 Pro.

```sh
git clone https://github.com/MaxHalford/cardinal
cd cardinal
git submodule update --init duckdb extension-ci-tools
make release
```

This gives `build/release/duckdb`, a DuckDB CLI with cardinal built in. Open a database file with it, and query as usual:

```sh
./build/release/duckdb my.duckdb
```

```sql
CREATE TABLE a AS SELECT range AS id, range % 10 AS x FROM range(1000);
CREATE TABLE b AS SELECT range % 1000 AS a_id, range % 7 AS y FROM range(10000);
SELECT count(*) FROM a, b WHERE a.id = b.a_id AND a.x = 3 AND b.y = 2;
```

The join's first run fits a model per table. They show up in `cardinal_models()`:

```sql
SELECT * FROM cardinal_models();
```

```
┌─────────────┬────────────┬───────────┬──────────────────┐
│ schema_name │ table_name │ row_count │     columns      │
├─────────────┼────────────┼───────────┼──────────────────┤
│ main        │ a          │      1000 │ ['F(b.a_id)', x] │
│ main        │ b          │     10000 │ [a_id, y]        │
└─────────────┴────────────┴───────────┴──────────────────┘
```

Models are saved in `~/.duckdb/extension_data/cardinal/`, never in your database, and refitted when a table's row count changes by more than 10%. A query that needs a model waits for its fit: milliseconds for small tables, about a second for large ones.

### Other clients

A client must be DuckDB v2.0.0 to load the extension built above. Allow unsigned extensions, then load the file:

```sh
duckdb -unsigned -c "LOAD 'build/release/extension/cardinal/cardinal.duckdb_extension';"
```

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.load_extension("build/release/extension/cardinal/cardinal.duckdb_extension")
```

Once cardinal is a community extension, this becomes:

```sql
INSTALL cardinal FROM community;
LOAD cardinal;
```

## Example

STATS-CEB's query 107 counts the comments, votes and badges of users, with filters on each:

```sql
SELECT count(*) FROM comments AS c, votes AS v, badges AS b, users AS u
WHERE u.Id = b.UserId AND u.Id = c.UserId AND u.Id = v.UserId
  AND c.Score = 1 AND c.CreationDate >= '2010-08-27 14:12:07'::TIMESTAMP
  AND v.VoteTypeId = 5
  AND v.CreationDate BETWEEN '2010-07-19'::TIMESTAMP AND '2014-09-13'::TIMESTAMP
  AND b.Date <= '2014-08-19 10:32:13'::TIMESTAMP AND u.Reputation >= 1;
```

| | DuckDB alone | With cardinal |
|---|---|---|
| Estimated result | 1,865 rows | 28 million rows |
| True result | 537 million rows | 537 million rows |
| Estimate error | 288,000× too low | 19× too low |
| Planning time | 1.0 ms | 1.1 ms |
| Total time | 12.9 s | 0.45 s |

Cardinal's estimate is still low, but close enough for DuckDB to pick a plan 29 times faster.

To compare on your own queries, turn cardinal off and on for the connection:

```sql
SET cardinal_estimator = 'off';
SET cardinal_estimator = 'on';
```

## Benchmarks

The same queries, with DuckDB v2.0.0 alone and with cardinal, on five standard benchmarks:

| Benchmark | Queries | Total time | Faster / same / slower queries | Typical estimate error |
|---|---|---|---|---|
| [STATS-CEB](https://github.com/Nathaniel-Han/End-to-End-CardEst-Benchmark) | 146 | **130 s → 67 s** | 42 / 102 / 2 | 23× → 1.9× |
| [JOB](https://github.com/gregrahn/join-order-benchmark) | 113 | 3.5 s → 2.9 s | 29 / 74 / 10 | 19× → 2.2× |
| [JOB-light](https://github.com/andreaskipf/learnedcardinalities) | 70 | 5.0 s → 4.8 s | 7 / 61 / 2 | 5.3× → 1.5× |
| [DSB](https://github.com/microsoft/dsb), scale factor 10 | 219 | 2.5 s → 2.7 s | 0 / 196 / 23 | 4.1× → 2.2× |
| TPC-H, scale factor 10 | 22 | 1.4 s → 1.3 s | 0 / 22 / 0 | 1.3× → 1.0× |

- **Total time**: the whole workload, each query's median over several runs. Planning time included.
- **Faster / same / slower**: a query is faster or slower when its time changes by more than 10% and 10 ms, the same otherwise.
- **Estimate error**: how far off the row counts DuckDB plans with are from the true ones, for every join DuckDB considers. The median: half the estimates are closer than this.

Estimates improve everywhere. Time follows where join order matters: STATS-CEB's slowest queries count billions of rows, and run twice as fast. DSB's queries take 10 ms each at this scale: its better plans make no difference, and small physical changes cost a few milliseconds. TPC-H, uniform data DuckDB already estimates well, is a check that cardinal does no harm.

Measured on an Apple M5 Pro. How, and more: [research/](research/).

## Limits

- Only DuckDB tables: queries that read Parquet or CSV files, or attached PostgreSQL tables, are planned by DuckDB alone.
- Joins in a cycle, joins on several columns at once, and conditions such as `a.x < b.y` are left to DuckDB's own estimates.
- Cardinal relies on DuckDB internals, so each DuckDB release needs a new build of it.

How it works, settings, opting in per connection, and fitting ahead of time: [docs/extension.md](docs/extension.md).

## Development

```sh
make release
./build/release/test/unittest "test/*"
```

Porting to a new DuckDB release: [docs/upgrading.md](docs/upgrading.md). The evaluation (benchmarks, accuracy, plan quality, runtime) lives in [research/](research/).

## License

[MIT](LICENSE).
