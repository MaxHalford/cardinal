#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {
namespace cardinal {

//! A query's rows, materialized: read by column and row.
class QueryRows {
public:
	explicit QueryRows(QueryResult &result);

	Value GetValue(idx_t column, idx_t row) const;
	idx_t RowCount() const;

private:
	unique_ptr<ColumnDataCollection> collection;
	unique_ptr<ColumnDataRowCollection> rows;
};

//! Marks this thread as running inner queries, until destroyed.
class InnerScope {
public:
	InnerScope();
	~InnerScope();

private:
	bool previous;
};

//! Whether this thread runs an inner query. Inner queries are planned too:
//! the extension leaves them alone.
bool InInnerQuery();

//! Run a query from inside the optimizer, on its own connection.
//! Null when nested in another inner query, or when the query fails.
unique_ptr<QueryRows> InnerQuery(ClientContext &context, const string &sql);

} // namespace cardinal
} // namespace duckdb
