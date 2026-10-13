#include "inner_query.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static thread_local bool inner = false;

InnerScope::InnerScope() : previous(inner) {
	inner = true;
}

InnerScope::~InnerScope() {
	inner = previous;
}

bool InInnerQuery() {
	return inner;
}

unique_ptr<MaterializedQueryResult> InnerQuery(ClientContext &context, const string &sql) {
	if (inner) {
		return nullptr;
	}

	InnerScope scope;
	unique_ptr<MaterializedQueryResult> result;
	try {
		Connection con(*context.db);
		result = con.Query(sql);
	} catch (std::exception &) {
		result = nullptr;
	}

	if (!result || result->HasError()) {
		return nullptr;
	}
	return result;
}

} // namespace duckdb
