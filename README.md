<div align="center">
    <h1>cardinal — better cardinality estimates for DuckDB</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB picks a join order from estimates of how many rows each join produces. Those estimates assume columns are independent, so on real data they can be off by orders of magnitude, and DuckDB picks slow plans.

`cardinal` is a DuckDB extension that learns Bayesian networks from your tables, and gives DuckDB their estimates instead. It needs no configuration: load it, and queries are planned with better numbers. Results never change, only plans.

<br clear="right"/>

## Benchmarks

The same queries, with DuckDB v2.0.0 alone and with cardinal, on five standard benchmarks:

| Benchmark | Queries | Total time | Faster / slower queries | Typical estimate error |
|---|---|---|---|---|
| [STATS-CEB](https://github.com/Nathaniel-Han/End-to-End-CardEst-Benchmark) | 146 | **130 s → 67 s** | 42 / 2 | 23× → 1.9× |
| [JOB](https://github.com/gregrahn/join-order-benchmark) | 113 | 3.5 s → 2.9 s | 29 / 10 | 19× → 2.2× |
| [JOB-light](https://github.com/andreaskipf/learnedcardinalities) | 70 | 5.0 s → 4.8 s | 7 / 2 | 5.3× → 1.5× |
| [DSB](https://github.com/microsoft/dsb), scale factor 10 | 219 | 2.3 s → 2.7 s | 0 / 19 | 4.1× → 2.1× |
| TPC-H, scale factor 10 | 22 | 1.3 s → 1.3 s | 0 / 0 | 1.3× → 1.0× |

- **Total time**: the whole workload, each query's median over several runs. Planning time included.
- **Faster / slower**: queries whose time changes by more than 10% and 10 ms.
- **Estimate error**: how far off the row counts DuckDB plans with are from the true ones, for every join DuckDB considers. The median: half the estimates are closer than this.

Estimates improve everywhere. Time follows where join order matters: STATS-CEB's slowest queries count billions of rows, and run twice as fast. DSB's queries take 10 ms each at this scale: its better plans make no difference, and small physical changes cost a few milliseconds. TPC-H, uniform data DuckDB already estimates well, is a check that cardinal does no harm.

Measured on an Apple M5 Pro. How, and more: [research/](research/).

## Installation

Once cardinal is listed in DuckDB's [community extensions](https://duckdb.org/community_extensions/) (in progress), install it from any DuckDB client:

```sql
INSTALL cardinal FROM community;
LOAD cardinal;
```

Until then, [build it](#building) and load the file, with unsigned extensions allowed:

```sh
duckdb -unsigned -c "LOAD 'build/release/extension/cardinal/cardinal.duckdb_extension';"
```

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.load_extension("build/release/extension/cardinal/cardinal.duckdb_extension")
```

The extension must be built for the same DuckDB version as the client loading it: v2.0.0.

## Usage

Once loaded, every query is planned with cardinal's estimates. Nothing else is needed.

Models are fitted the first time a query needs them, saved, and refreshed when tables change. How it behaves, where models live, settings and limits: [docs/extension.md](docs/extension.md).

## Building

```sh
git submodule update --init
make release      # DuckDB with the extension: build/release/duckdb
./build/release/test/unittest "test/*"
```

Each DuckDB release needs a port: see [docs/upgrading.md](docs/upgrading.md).

## Research

The evaluation (benchmarks, accuracy, plan quality, runtime) lives in [research/](research/).
