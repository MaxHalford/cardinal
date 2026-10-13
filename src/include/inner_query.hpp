#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

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
unique_ptr<MaterializedQueryResult> InnerQuery(ClientContext &context, const string &sql);

} // namespace duckdb
