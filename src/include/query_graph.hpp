//===----------------------------------------------------------------------===//
// The join graph of a query, as DuckDB's join order optimizer will see it:
// table scans, the filters on each, the equi-joins between them, and the
// filters across them.
//
//      title ──── t.id = mi.movie_id ──── movie_info
//        │
//   t.id = mc.movie_id
//        │
//   movie_companies
//
// Built on a copy of the plan after DuckDB's filter pushdown, so filters and
// join conditions are where the optimizer will find them.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {
namespace cardinal {

//! A table scan of the query.
struct GraphRelation {
	//! The scan in the analyzed copy: same table_index as in the real plan.
	reference<LogicalGet> get;
	//! Filters left above the scan, besides its table filters.
	vector<unique_ptr<Expression>> filters;
};

//! A column of a relation: (relation, table column index).
using GraphColumn = pair<idx_t, idx_t>;

//! An equi-join condition between columns of two relations.
struct GraphEdge {
	GraphColumn left;
	GraphColumn right;
};

//! A filter over several relations, e.g. (d.kind = 1 AND f.flag = 2) OR (d.kind = 2 AND f.flag = 3).
//! Its column references point to the scans' own columns.
struct GraphFilter {
	unique_ptr<Expression> expr;
	//! Bit i: relation i.
	uint64_t relations;
	//! Some column could not be traced to a scan: sets with it are out of reach.
	bool resolved;
	//! An OR, which DuckDB's join formula counts with a default selectivity.
	bool is_or;
};

struct QueryGraph {
	//! The analyzed copy: it owns the scans.
	unique_ptr<LogicalOperator> plan;
	vector<GraphRelation> relations;
	vector<GraphEdge> edges;
	//! Columns made equal by the edges: DuckDB's equivalence classes.
	vector<vector<GraphColumn>> classes;
	//! The class of each edge.
	vector<idx_t> edge_class;
	//! Filters across relations, besides the edges.
	vector<GraphFilter> filters;
};

//! The join graph of a plan. Empty when the plan cannot be copied.
QueryGraph BuildGraph(Optimizer &optimizer, LogicalOperator &plan);

} // namespace cardinal
} // namespace duckdb
