# Patches

## `duckdb-v1.5.6-ce-trace.patch`

Applies to DuckDB v1.5.6 (commit 069cc9f). It logs every cardinality question that the join-order optimizer asks, as JSON lines, to the file named by `DUCKDB_CE_TRACE`. It only logs: it does not change optimizer decisions. If the variable is not set, nothing is written.

```sh
git clone https://github.com/duckdb/duckdb vendor/duckdb
cd vendor/duckdb && git checkout v1.5.6
git apply ../../patches/duckdb-v1.5.6-ce-trace.patch
make release
DUCKDB_CE_TRACE=trace.jsonl build/release/duckdb my.db -c "EXPLAIN SELECT ..."
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
