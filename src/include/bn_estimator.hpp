#pragma once

#include "model.hpp"

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {
namespace cardinal {

//! A scan's filters on a model: column references point to model columns.
struct PreparedFilters {
	vector<unique_ptr<Expression>> filters;
	//! Type of each model column.
	vector<LogicalType> types;
	//! The filters as text: a cache key.
	string key;
};

//! Fanout powers: model column -> exponent.
using Powers = map<idx_t, idx_t>;

//! The scan's table filters and the filters above it, on the model. False when a
//! filter reads a column outside the model.
bool PrepareFilters(LogicalGet &get, const vector<unique_ptr<Expression>> &filters, const Model &model,
                    PreparedFilters &out);

//! E[1(filters) · Π F^k] under the model. False when a filter is out of reach.
bool Expect(ClientContext &context, const Model &model, const PreparedFilters &filters, const Powers &powers,
            double &result);

//! The table columns the scan's filters read: its table filters and the filters above it.
vector<string> FilteredColumns(LogicalGet &get, const vector<unique_ptr<Expression>> &filters);

//! Rows of a scan that pass its filters, by the table's model: rows x P(filters).
//! Invalid when the table has no model, or a filter is out of its reach.
optional_idx BNEstimate(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters,
                        idx_t rows);

} // namespace cardinal
} // namespace duckdb
