<div align="center">
    <h1>cardinal — better cardinality estimates for DuckDB</h1>
</div>
</br>

<img style="padding-bottom: 20px" src="logo.png" width="30%" alt="A duck dressed as a cardinal, wearing a medallion with a bar chart" align="right" />

DuckDB picks a join order from estimates of how many rows each join produces. Those estimates assume columns are independent, so on real data they can be off by orders of magnitude, and DuckDB picks slow plans.

`cardinal` is a DuckDB extension that learns Bayesian networks from your tables, and gives DuckDB their estimates instead. It needs no configuration: load it, and queries are planned with better numbers. Results never change, only plans.

<br clear="right"/>

## Usage

```sql
LOAD cardinal;
```

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
