# Patches

## `duckdb-v2.0.0-ce-trace.patch`

Applies to DuckDB v2.0.0 (commit d54cbbe). It logs every cardinality question that the join-order optimizer asks, as JSON lines, to the file named by `DUCKDB_CE_TRACE`. Logging does not change optimizer decisions. If the variable is not set, nothing is written.

With `DUCKDB_CE_INJECT`, the optimizer reads estimates from that file instead of computing them, one per line: `<inv> <relation list> <estimate>`, e.g. `1 [0,2] 1234.5`. Questions missing from the file keep DuckDB's estimate. Invocation ids count from 1 per process: run one query per process. An injected base cardinality also enters DuckDB's join formula. Injected questions are logged with `"injected":true`.

```sh
git clone https://github.com/duckdb/duckdb duckdb
cd duckdb && git checkout v2.0.0
git apply ../duckdb-v2.0.0-ce-trace.patch
make release
DUCKDB_CE_TRACE=trace.jsonl build/release/duckdb my.db -c "EXPLAIN SELECT ..."
DUCKDB_CE_INJECT=estimates.txt DUCKDB_CE_TRACE=trace.jsonl build/release/duckdb my.db -c "EXPLAIN SELECT ..."
```

Events (one JSON object per line, `inv` = optimizer invocation id):

| `ev` | Meaning |
|---|---|
| `invocation` | One `JoinOrderOptimizer::Optimize` call: parent, depth, reorderable flag, relations (table or derived, pushed filters as SQL, DuckDB estimate, distinct counts) and filters (relation sets, join type, SQL). Written after the children's events. |
| `tdom` | One equivalence set of join columns and its total domain. |
| `phase` | Enumeration mode: `exact`, `greedy_threshold`, `greedy_pair_limit`, `cross_products`. |
| `question` | One cardinality request: relation set, estimate, `cache_hit`, caller (`leaf`, `cost_model`, `join_node`), and numerator/denominator when computed. |
| `plan` | The chosen join tree. |

In SQL strings, a column is written `r<relation id>.<column>`. Columns of derived relations are written `r<id>.col_<table index>_<column index>`.

v2.0.0 differences from the v1.5.6 trace:

- A question's estimate is DuckDB's final one, after its OR-filter selectivities. Injection replaces that final estimate.
- `tdom`: `hll` is the HLL or exact distinct count (`has_hll`), `no_hll` the min-max or row-count fallback otherwise.
- Table filters are all expression filters in v2: `type` is `OPTIONAL_FILTER` or `EXPRESSION_FILTER`. Their SQL leaves out optional parts.
- Relation columns add `source` (`HLL`, `EXACT`, `MIN_MAX`, `CARDINALITY`). Their names are DuckDB's, e.g. `SEQ_SCAN.kind`.
- v2 keeps a FILTER above a scan even when its expression is also a graph filter. The trace lists it once, as a graph filter, as v1.5.6 did.
