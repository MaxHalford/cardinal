<div align="center">
    <h1>cardinal — better cardinality estimates for DuckDB</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB picks a join order from estimates of how many rows each join produces. Those estimates assume columns are independent, so on real data they can be off by orders of magnitude, and DuckDB picks slow plans.

`cardinal` is a DuckDB extension that learns Bayesian networks from your tables, and gives DuckDB their estimates instead. It needs no configuration: load it, and queries are planned with better numbers. Results never change, only plans.

<br clear="right"/>

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
