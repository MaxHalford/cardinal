#include "exact.hpp"
#include "inner_query.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

//! Counts by query text, shared by connections. Filter pushdown asks several times.
static mutex cache_lock;
static unordered_map<string, idx_t> cache;

static string CountQuery(LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	vector<string> conditions;
	for (auto &entry : get.table_filters.filters) {
		// Optional filters only skip data: they never remove rows.
		if (entry.second->filter_type == TableFilterType::OPTIONAL_FILTER) {
			continue;
		}
		auto column = KeywordHelper::WriteOptionallyQuoted(get.names[entry.first]);
		conditions.push_back(entry.second->ToString(column));
	}
	for (auto &expr : filters) {
		conditions.push_back(expr->ToString());
	}

	auto &table = *get.GetTable();
	auto sql = "SELECT count(*) FROM " + KeywordHelper::WriteOptionallyQuoted(table.ParentCatalog().GetName()) + "." +
	           KeywordHelper::WriteOptionallyQuoted(table.ParentSchema().name) + "." +
	           KeywordHelper::WriteOptionallyQuoted(table.name);
	if (!conditions.empty()) {
		sql += " WHERE " + StringUtil::Join(conditions, " AND ");
	}
	return sql;
}

optional_idx ExactCount(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	auto sql = CountQuery(get, filters);

	{
		lock_guard<mutex> guard(cache_lock);
		auto hit = cache.find(sql);
		if (hit != cache.end()) {
			return hit->second;
		}
	}

	auto result = InnerQuery(context, sql);
	if (!result) {
		return optional_idx();
	}

	auto rows = idx_t(result->GetValue(0, 0).GetValue<int64_t>());
	lock_guard<mutex> guard(cache_lock);
	cache[sql] = rows;
	return rows;
}

} // namespace duckdb
