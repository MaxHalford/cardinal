<div align="center">
    <h1>cardinal — better cardinality estimates for DuckDB</h1>
    <a href="https://github.com/MaxHalford/cardinal/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/MaxHalford/cardinal/actions/workflows/MainDistributionPipeline.yml/badge.svg" alt="CI"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"></a>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB picks a join order from estimates of how many rows each join produces. Those estimates assume columns are independent and every join key matches as many rows as any other, so on real data they can be off by orders of magnitude, and DuckDB picks slow plans.

`cardinal` is a DuckDB extension that learns Bayesian networks from your tables, and gives DuckDB their estimates instead. It needs no configuration: load it, and queries are planned with better numbers. Results never change, only plans. On the STATS-CEB benchmark, total runtime drops from 118 s to 70 s.

<br clear="right"/>

## Quick start

Cardinal is a DuckDB [community extension](https://duckdb.org/community_extensions/extensions/cardinal), for DuckDB v2.0.0. Install and load it from any DuckDB client:

```sql
INSTALL cardinal FROM community;
LOAD cardinal;
```

Then query as usual. With a database file, e.g. `duckdb my.duckdb`:

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

Models are saved in `~/.duckdb/extension_data/cardinal/`, never in your database, and refitted when a table's row count changes by more than 10%, or when it is rewritten or altered. A query that needs a model waits for its fit: milliseconds for small tables, about a second for large ones.

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

### How the plans differ

DuckDB runs each join as a hash join: it stores one input, the build side, in a hash table, and streams the other input through it. It builds on the side it thinks is smaller. Each join shows its estimated and true rows:

```
DuckDB alone                          estimated          true
⋈ u.Id = v.UserId                         1,865   537,352,263
├── users                                40,325        40,325
└── build: ⋈ v.UserId = c.UserId          1,711   537,352,263
    ├── votes                            25,236        33,690
    └── build: ⋈ b.UserId = c.UserId      2,508     3,203,614
        ├── badges                       15,970        77,535
        └── build: comments               5,811        24,733

With cardinal                         estimated          true
⋈ v.UserId = u.Id                    28,146,412   537,352,263
├── ⋈ v.UserId = c.UserId               610,081     2,087,379
│   ├── votes                            46,598        33,690
│   └── build: comments                  44,187        24,733
└── build: ⋈ b.UserId = u.Id            155,707        77,535
    ├── badges                           76,106        77,535
    └── build: users                      6,905        40,325
```

Alone, DuckDB thinks joining comments, badges and votes gives 1,711 rows, fewer than the 40,325 users. So it builds the last hash table on that join, which stores 537 million rows. With cardinal, DuckDB sees that this join explodes. It joins votes with comments (2 million rows) and badges with users (78,000 rows), and builds on the smaller one. The last join then streams its 537 million rows into `count(*)` and stores none of them.

### Why DuckDB's estimate is off

DuckDB estimates with [almost no statistics](https://blobs.duckdb.org/papers/tom-ebergen-msc-thesis-join-order-optimization-with-almost-no-statistics.pdf):

- `x = 'a'` keeps the rows divided by the distinct values of `x`.
- Any other filter keeps 20% of the rows.
- A join gives the product of its inputs, divided by the distinct join keys.

The join rule assumes every key matches as many rows as any other. Here, an average user has 0.6 matching comments, 0.8 votes and 1.9 badges, so the 40,325 users would give about 40,000 rows. But active users comment, vote and earn badges at once, and their counts multiply. User 805 alone gives 1,845 × 482 × 294 = 261 million rows. Ten users give 97% of the result.

### Why cardinal's estimate is closer

Cardinal fits a [Bayesian network](https://en.wikipedia.org/wiki/Bayesian_network) per table. It learns how the columns that queries filter on correlate, instead of treating each filter alone. Following [BayesCard](https://arxiv.org/abs/2012.14743), each table also gets fanout columns: for each user, the count of its comments, votes and badges. The network learns these counts together, so it knows that a user with many comments also has many votes. A join's size is then the sum, over the users that pass the filters, of comments × votes × badges.

## Benchmarks

The same queries, with DuckDB v2.0.0 alone and with cardinal, on five standard benchmarks:

| Benchmark | Queries | Total time | Faster / same / slower queries | Median estimate error |
|---|---|---|---|---|
| [STATS-CEB](https://github.com/Nathaniel-Han/End-to-End-CardEst-Benchmark) | 146 | **118 s → 70 s** | 38 / 107 / 1 | 23× → 1.8× |
| [JOB](https://github.com/gregrahn/join-order-benchmark) | 113 | 4.0 s → 3.5 s | 32 / 64 / 17 | 19× → 2.2× |
| [JOB-light](https://github.com/andreaskipf/learnedcardinalities) | 70 | 5.3 s → 5.1 s | 11 / 57 / 2 | 5.3× → 1.5× |
| [DSB](https://github.com/microsoft/dsb), scale factor 10 | 219 | 2.8 s → 3.2 s | 0 / 196 / 23 | 4.1× → 2.0× |
| TPC-H, scale factor 10 | 22 | 1.4 s → 1.4 s | 1 / 20 / 1 | 1.3× → 1.0× |

- **Total time**: the whole workload, each query's median over several runs. Planning time included.
- **Faster / same / slower**: a query is faster or slower when its time changes by more than 10% and 10 ms, the same otherwise.
- **Median estimate error**: how far off the row counts DuckDB plans with are from the true ones, as a ratio, over every join DuckDB considers. Half the estimates are closer than this.

Estimates improve everywhere. Time follows where join order matters: STATS-CEB's slowest queries count billions of rows, and run twice as fast. DSB's queries take 10 ms each at this scale: its better plans make no difference, and small physical changes cost a few milliseconds. TPC-H, uniform data DuckDB already estimates well, is a check that cardinal does no harm.

Measured on an Apple M5 Pro. How, and more: [research/](research/).

## Limits

- Only DuckDB tables: queries that read Parquet or CSV files, or attached PostgreSQL tables, are planned by DuckDB alone.
- Joins in a cycle, joins on several columns at once, and conditions such as `a.x < b.y` are left to DuckDB's own estimates.
- Cardinal relies on DuckDB internals, so each DuckDB release needs a new build of it.

How it works, settings, opting in per connection, and fitting ahead of time: [docs/extension.md](docs/extension.md).

## Development

Building needs what [DuckDB's build](https://duckdb.org/docs/stable/dev/building/overview) needs: Git, CMake and a C++ compiler. It compiles DuckDB v2.0.0 itself: about 15 minutes on an Apple M5 Pro.

```sh
git clone https://github.com/MaxHalford/cardinal
cd cardinal
git submodule update --init duckdb extension-ci-tools
make release
./build/release/test/unittest "test/*"
```

`build/release/duckdb` is a DuckDB CLI with cardinal built in.

Porting to a new DuckDB release: [docs/upgrading.md](docs/upgrading.md). The evaluation (benchmarks, accuracy, plan quality, runtime) lives in [research/](research/).

## License

[MIT](LICENSE).
