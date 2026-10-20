#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

//! A query over the rows of a scan that pass its filters: its table filters, and the filters
//! above it. e.g. select "count(*)": SELECT count(*) FROM "imdb"."main"."title" WHERE ...
string ScanQuery(LogicalGet &get, const vector<unique_ptr<Expression>> &filters, const string &select);

//! Rows of a scan that pass its filters, counted with a query.
//! A test oracle: it isolates the scan hook from estimation errors.
optional_idx ExactCount(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters);

} // namespace duckdb
