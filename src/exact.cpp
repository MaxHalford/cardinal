#include "exact.hpp"
#include "bn_estimator.hpp"
#include "inner_query.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {
namespace cardinal {

//! Counts by query text, shared by connections. Filter pushdown asks several times.
static mutex cache_lock;
static unordered_map<string, idx_t> cache;

//! A column of the scan as SQL, e.g. "group" quoted. Empty for a virtual column, e.g. rowid.
static string ColumnSQL(LogicalGet &get, const ColumnIndex &index) {
	if (index.IsVirtualColumn() || index.GetPrimaryIndex() >= get.names.size()) {
		return "";
	}
	return KeywordHelper::WriteOptionallyQuoted(get.names[index.GetPrimaryIndex()].GetIdentifierName());
}

//! A filter on the scan's columns as SQL. Empty when it has no SQL form, e.g. a dynamic filter.
static string FilterSQL(LogicalGet &get, unique_ptr<Expression> expr) {
	if (!expr) {
		return "";
	}
	bool named = true;
	auto &ids = get.GetColumnIds();
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &slot) {
		    auto &binding = ref.Binding();
		    auto column = binding.table_index == get.table_index && binding.column_index < ids.size()
		                      ? ColumnSQL(get, ids[binding.column_index])
		                      : "";
		    named = named && !column.empty();
		    ref.SetAlias(Identifier(column));
	    });
	return named ? expr->ToString() : "";
}

string ScanQuery(LogicalGet &get, const vector<unique_ptr<Expression>> &filters, const string &select) {
	vector<string> conditions;
	for (auto &entry : get.table_filters) {
		// Optional filters only skip data: they never remove rows.
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			continue;
		}
		// Filters are keyed by the scan's columns, e.g. #0 is info when the scan reads (info, id).
		auto &index = get.GetColumnIndex(entry.GetIndex());
		auto column = ColumnSQL(get, index);
		if (column.empty()) {
			return "";
		}
		BoundReferenceExpression ref(Identifier(column), get.returned_types[index.GetPrimaryIndex()], 0);
		auto required = RequiredPart(*entry.Filter().ToExpression(ref));
		if (!required) {
			return "";
		}
		conditions.push_back(required->ToString());
	}
	for (auto &expr : filters) {
		auto condition = FilterSQL(get, RequiredPart(*expr));
		if (condition.empty()) {
			return "";
		}
		conditions.push_back(condition);
	}

	auto &table = *get.GetTable();
	auto sql = "SELECT " + select + " FROM " +
	           KeywordHelper::WriteOptionallyQuoted(table.ParentCatalog().GetName().GetIdentifierName()) + "." +
	           KeywordHelper::WriteOptionallyQuoted(table.ParentSchema().name.GetIdentifierName()) + "." +
	           KeywordHelper::WriteOptionallyQuoted(table.name.GetIdentifierName());
	if (!conditions.empty()) {
		sql += " WHERE " + StringUtil::Join(conditions, " AND ");
	}
	return sql;
}

optional_idx ExactCount(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	auto sql = ScanQuery(get, filters, "count(*)");
	if (sql.empty()) {
		return optional_idx();
	}
	// The table's row count too: a count is stale once rows change.
	auto &table = *get.GetTable();
	auto key = sql + " -- " + to_string(table.IsDuckTable() ? table.GetStorage().GetTotalRows() : 0);

	{
		lock_guard<mutex> guard(cache_lock);
		auto hit = cache.find(key);
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
	cache[key] = rows;
	return rows;
}

} // namespace cardinal
} // namespace duckdb
