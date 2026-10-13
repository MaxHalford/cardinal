#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

//! Rows of a scan that pass its filters, counted with a query.
//! A test oracle: it isolates the scan hook from estimation errors.
optional_idx ExactCount(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters);

} // namespace duckdb
