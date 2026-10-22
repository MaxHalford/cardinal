#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {
namespace cardinal {

//! A query over the rows of a scan that pass its filters: its table filters, and the filters
//! above it. e.g. select "count(*)": SELECT count(*) FROM "imdb"."main"."title" WHERE ...
//! Empty when a filter has no SQL form, e.g. a dynamic filter, or reads a virtual column.
string ScanQuery(LogicalGet &get, const vector<unique_ptr<Expression>> &filters, const string &select);

//! Rows of a scan that pass its filters, counted with a query.
//! A test oracle: it isolates the scan hook from estimation errors.
optional_idx ExactCount(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters);

} // namespace cardinal
} // namespace duckdb
