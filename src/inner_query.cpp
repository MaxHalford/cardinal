#include "inner_query.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {
namespace cardinal {

QueryRows::QueryRows(QueryResult &result) : collection(result.TakeCollection()) {
	rows = make_uniq<ColumnDataRowCollection>(*collection);
}

Value QueryRows::GetValue(idx_t column, idx_t row) const {
	return rows->GetValue(column, row);
}

idx_t QueryRows::RowCount() const {
	return collection->Count();
}

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

unique_ptr<QueryRows> InnerQuery(ClientContext &context, const string &sql) {
	if (inner) {
		return nullptr;
	}

	InnerScope scope;
	try {
		Connection con(*context.db);
		auto result = con.Query(sql);
		if (result->HasError()) {
			return nullptr;
		}
		return make_uniq<QueryRows>(*result);
	} catch (std::exception &) {
		return nullptr;
	}
}

} // namespace cardinal
} // namespace duckdb
