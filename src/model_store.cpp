//===----------------------------------------------------------------------===//
// Models live in the __cardinal schema of the database they describe:
//
//   tables          table_name, row_count
//   columns         table_name, column_name, parent, members
//   states          table_name, column_name, state, value, kind
//   probabilities   table_name, column_name, state, parent_state, probability
//
// Values are text: they are cast to the table column's type when read.
//===----------------------------------------------------------------------===//

#include "model_store.hpp"
#include "inner_query.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

static string Quote(const string &text) {
	return "'" + StringUtil::Replace(text, "'", "''") + "'";
}

static StateKind ParseKind(const string &kind) {
	if (kind == "null") {
		return StateKind::NULL_VALUE;
	}
	if (kind == "other") {
		return StateKind::OTHER;
	}
	return StateKind::VALUE;
}

//! e.g. SELECT ... FROM "stats".__cardinal.states WHERE table_name = 'users' ORDER BY ...
static unique_ptr<MaterializedQueryResult> Select(ClientContext &context, TableCatalogEntry &table,
                                                  const string &columns, const string &from, const string &order,
                                                  const string &where = "") {
	auto catalog = KeywordHelper::WriteOptionallyQuoted(table.ParentCatalog().GetName());
	auto sql = "SELECT " + columns + " FROM " + catalog + "." + MODEL_SCHEMA + "." + from + " WHERE table_name = " +
	           Quote(table.name) + (where.empty() ? "" : " AND " + where) + (order.empty() ? "" : " ORDER BY " + order);
	return InnerQuery(context, sql);
}

shared_ptr<Model> ReadModel(ClientContext &context, TableCatalogEntry &table) {
	auto tables = Select(context, table, "row_count", "tables", "");
	if (!tables || tables->RowCount() == 0) {
		return nullptr;
	}
	auto rows = idx_t(tables->GetValue(0, 0).GetValue<int64_t>());

	auto columns_result = Select(context, table, "column_name, parent, members", "columns", "");
	auto states = Select(context, table, "column_name, state, value, kind", "states", "column_name, state");
	auto probabilities = Select(context, table, "column_name, state, parent_state, probability", "probabilities", "");
	if (!columns_result || !states || !probabilities) {
		return nullptr;
	}

	vector<ModelColumn> columns(columns_result->RowCount());
	case_insensitive_map_t<idx_t> index;
	for (idx_t i = 0; i < columns.size(); i++) {
		columns[i].name = columns_result->GetValue(0, i).ToString();
		auto members = columns_result->GetValue(2, i);
		if (!members.IsNull()) {
			columns[i].members = idx_t(members.GetValue<int64_t>());
		}
		index[columns[i].name] = i;
	}
	for (idx_t i = 0; i < columns.size(); i++) {
		auto parent = columns_result->GetValue(1, i);
		if (!parent.IsNull()) {
			columns[i].parent = index.at(parent.ToString());
		}
	}

	// States arrive in order: state i is the i-th of its column.
	// Fanout columns, e.g. F(badges.UserId), are not table columns: their states are mean counts.
	auto &table_columns = table.GetColumns();
	for (auto &column : columns) {
		column.type = table_columns.ColumnExists(column.name) ? table_columns.GetColumn(column.name).Type()
		                                                      : LogicalType::DOUBLE;
	}
	for (idx_t i = 0; i < states->RowCount(); i++) {
		auto &column = columns[index.at(states->GetValue(0, i).ToString())];
		auto kind = ParseKind(states->GetValue(3, i).ToString());
		Value value = states->GetValue(2, i);
		if (kind == StateKind::VALUE && !value.DefaultTryCastAs(column.type)) {
			return nullptr;
		}
		column.kinds.push_back(kind);
		column.values.push_back(kind == StateKind::VALUE ? value : Value());
	}

	for (idx_t i = 0; i < probabilities->RowCount(); i++) {
		auto &column = columns[index.at(probabilities->GetValue(0, i).ToString())];
		auto parent_state = probabilities->GetValue(2, i);
		Probability entry;
		entry.state = idx_t(probabilities->GetValue(1, i).GetValue<int32_t>());
		entry.parent_state = parent_state.IsNull() ? 0 : idx_t(parent_state.GetValue<int32_t>());
		entry.p = probabilities->GetValue(3, i).GetValue<double>();
		column.probabilities.push_back(entry);
	}

	return make_shared_ptr<Model>(rows, std::move(columns));
}

} // namespace duckdb

namespace duckdb {

static const char *SCHEMA_SQL = R"(
CREATE SCHEMA IF NOT EXISTS %s;
CREATE TABLE IF NOT EXISTS %s.tables (table_name VARCHAR, row_count BIGINT);
CREATE TABLE IF NOT EXISTS %s.columns (table_name VARCHAR, column_name VARCHAR, parent VARCHAR, members BIGINT);
CREATE TABLE IF NOT EXISTS %s.states (table_name VARCHAR, column_name VARCHAR, state INTEGER, value VARCHAR, kind VARCHAR);
CREATE TABLE IF NOT EXISTS %s.probabilities (table_name VARCHAR, column_name VARCHAR, state INTEGER, parent_state INTEGER, probability DOUBLE);
)";

static const char *KindName(StateKind kind) {
	switch (kind) {
	case StateKind::NULL_VALUE:
		return "null";
	case StateKind::OTHER:
		return "other";
	default:
		return "value";
	}
}

static void Check(unique_ptr<MaterializedQueryResult> result) {
	if (result->HasError()) {
		throw InvalidInputException("cardinal: %s", result->GetError());
	}
}

//! The store lock, in DuckDB's object cache: one per database.
struct StoreLockEntry : public ObjectCacheEntry {
	static string ObjectType() {
		return "cardinal_store_lock";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
	mutex lock;
};

mutex &StoreLock(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<StoreLockEntry>("cardinal:store_lock")->lock;
}

void SaveModel(Connection &con, TableCatalogEntry &table, const Model &model) {
	lock_guard<mutex> guard(StoreLock(*con.context));
	auto catalog_name = table.ParentCatalog().GetName();
	auto catalog = KeywordHelper::WriteOptionallyQuoted(catalog_name);
	auto name = table.name;

	// One transaction: a failure leaves the stored model as it was.
	con.BeginTransaction();
	try {
		auto prefix = catalog + "." + MODEL_SCHEMA;
		auto schema = con.Query(StringUtil::Format(SCHEMA_SQL, prefix, prefix, prefix, prefix, prefix));
		if (schema->HasError()) {
			throw InvalidInputException("cardinal: %s", schema->GetError());
		}
		for (auto from : {"tables", "columns", "states", "probabilities"}) {
			Check(con.Query("DELETE FROM " + prefix + "." + from + " WHERE table_name = " + Quote(name)));
		}

		Appender tables(con, catalog_name, MODEL_SCHEMA, "tables");
		tables.AppendRow(Value(name), Value::BIGINT(int64_t(model.Rows())));
		tables.Close();

		Appender columns(con, catalog_name, MODEL_SCHEMA, "columns");
		Appender states(con, catalog_name, MODEL_SCHEMA, "states");
		Appender probabilities(con, catalog_name, MODEL_SCHEMA, "probabilities");
		auto &cols = model.Columns();
		for (auto &col : cols) {
			auto parent = col.parent.IsValid() ? Value(cols[col.parent.GetIndex()].name) : Value(LogicalType::VARCHAR);
			auto members = col.members.IsValid() ? Value::BIGINT(int64_t(col.members.GetIndex())) : Value(LogicalType::BIGINT);
			columns.AppendRow(Value(name), Value(col.name), parent, members);

			for (idx_t s = 0; s < col.values.size(); s++) {
				auto value = col.kinds[s] == StateKind::VALUE ? col.values[s].DefaultCastAs(LogicalType::VARCHAR)
				                                              : Value(LogicalType::VARCHAR);
				states.AppendRow(Value(name), Value(col.name), Value::INTEGER(int32_t(s)), value,
				                 Value(KindName(col.kinds[s])));
			}
			for (auto &entry : col.probabilities) {
				auto parent_state =
				    col.parent.IsValid() ? Value::INTEGER(int32_t(entry.parent_state)) : Value(LogicalType::INTEGER);
				probabilities.AppendRow(Value(name), Value(col.name), Value::INTEGER(int32_t(entry.state)), parent_state,
				                        Value::DOUBLE(entry.p));
			}
		}
		columns.Close();
		states.Close();
		probabilities.Close();
		con.Commit();
	} catch (std::exception &) {
		con.Rollback();
		throw;
	}
}

} // namespace duckdb
