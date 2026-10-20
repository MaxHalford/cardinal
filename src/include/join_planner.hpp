//===----------------------------------------------------------------------===//
// Join estimates for DuckDB's own optimizer, through the only numbers it
// reads: a row count per scan, a distinct count per join column.
//
// DuckDB estimates a set S of relations as
//
//    log card(S) = Σ_{R ∈ S} n_R − Σ_{edges e joining S} d_class(e) − log(1 + unused)
//
// n_R: log rows of R after filters. d_c: log distinct values of the class of
// columns c (the largest of its columns). Edges join S in decreasing d order;
// once S is connected, each class with edges left counts as unused.
//
// Per query, n and d are fitted (least squares) so that this formula matches
// the fanout estimates of the connected sets. DuckDB then plans as usual.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"

namespace duckdb {

//! What the scan hooks report to DuckDB for one query.
struct JoinTargets {
	//! Rows after filters, by scan table_index.
	unordered_map<idx_t, double> rows;
	//! Distinct values, by scan table_index and table column.
	map<pair<idx_t, idx_t>, idx_t> distinct;
	//! Leave the query to DuckDB, scans included: its plan was not believed worse.
	bool hands_off = false;
};

//! Targets for the plan's joins. Empty when the plan has no join to estimate.
JoinTargets PlanJoins(ClientContext &context, Optimizer &optimizer, LogicalOperator &plan);

} // namespace duckdb
