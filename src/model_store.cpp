//===----------------------------------------------------------------------===//
// Models in the store (store.hpp), one row set per database and table:
//
//   tables          database, table_name, row_count
//   columns         database, table_name, column_name, parent, members
//   states          database, table_name, column_name, state, value, kind
//   probabilities   database, table_name, column_name, state, parent_state, probability
//
// Values are text: they are cast to the table column's type when read.
//===----------------------------------------------------------------------===//

#include "model_store.hpp"
#include "store.hpp"

#include "duckdb/main/appender.hpp"

namespace duckdb {

static StateKind ParseKind(const string &kind) {
	if (kind == "null") {
		return StateKind::NULL_VALUE;
	}
	if (kind == "other") {
		return StateKind::OTHER;
	}
	return StateKind::VALUE;
}

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

//! e.g. SELECT ... FROM states WHERE database = $1 AND table_name = $2 ORDER BY ...
static unique_ptr<MaterializedQueryResult> Select(Connection &con, const string &database, TableCatalogEntry &table,
                                                  const string &columns, const string &from,
                                                  const string &order = "") {
	return StoreQuery(con,
	                  "SELECT " + columns + " FROM " + from + " WHERE database = $1 AND table_name = $2" +
	                      (order.empty() ? "" : " ORDER BY " + order),
	                  {Value(database), Value(table.name)});
}

static shared_ptr<Model> Read(Connection &con, const string &database, TableCatalogEntry &table) {
	auto tables = Select(con, database, table, "row_count", "tables");
	if (tables->RowCount() == 0) {
		return nullptr;
	}
	auto rows = idx_t(tables->GetValue(0, 0).GetValue<int64_t>());

	auto columns_result = Select(con, database, table, "column_name, parent, members", "columns");
	auto &columns_rows = *columns_result;
	vector<ModelColumn> columns(columns_rows.RowCount());
	case_insensitive_map_t<idx_t> index;
	for (idx_t i = 0; i < columns.size(); i++) {
		columns[i].name = columns_rows.GetValue(0, i).ToString();
		auto members = columns_rows.GetValue(2, i);
		if (!members.IsNull()) {
			columns[i].members = idx_t(members.GetValue<int64_t>());
		}
		index[columns[i].name] = i;
	}
	for (idx_t i = 0; i < columns.size(); i++) {
		auto parent = columns_rows.GetValue(1, i);
		if (!parent.IsNull()) {
			columns[i].parent = index.at(parent.ToString());
		}
	}

	// Fanout columns, e.g. F(badges.UserId), are not table columns: their states are mean counts.
	auto &table_columns = table.GetColumns();
	for (auto &column : columns) {
		column.type = table_columns.ColumnExists(column.name) ? table_columns.GetColumn(column.name).Type()
		                                                      : LogicalType::DOUBLE;
	}

	// States arrive in order: state i is the i-th of its column.
	auto states_result = Select(con, database, table, "column_name, state, value, kind", "states", "column_name, state");
	auto &states = *states_result;
	for (idx_t i = 0; i < states.RowCount(); i++) {
		auto &column = columns[index.at(states.GetValue(0, i).ToString())];
		auto kind = ParseKind(states.GetValue(3, i).ToString());
		Value value = states.GetValue(2, i);
		if (kind == StateKind::VALUE && !value.DefaultTryCastAs(column.type)) {
			return nullptr;
		}
		column.kinds.push_back(kind);
		column.values.push_back(kind == StateKind::VALUE ? value : Value());
	}

	auto probabilities_result =
	    Select(con, database, table, "column_name, state, parent_state, probability", "probabilities");
	auto &probabilities = *probabilities_result;
	for (idx_t i = 0; i < probabilities.RowCount(); i++) {
		auto &column = columns[index.at(probabilities.GetValue(0, i).ToString())];
		auto parent_state = probabilities.GetValue(2, i);
		Probability entry;
		entry.state = idx_t(probabilities.GetValue(1, i).GetValue<int32_t>());
		entry.parent_state = parent_state.IsNull() ? 0 : idx_t(parent_state.GetValue<int32_t>());
		entry.p = probabilities.GetValue(3, i).GetValue<double>();
		column.probabilities.push_back(entry);
	}
	return make_shared_ptr<Model>(rows, std::move(columns));
}

shared_ptr<Model> ReadModel(ClientContext &context, TableCatalogEntry &table) {
	auto database = DatabaseKey(table.ParentCatalog());
	if (database.empty()) {
		return nullptr;
	}
	shared_ptr<Model> model;
	WithStore(context, [&](Connection &con) { model = Read(con, database, table); });
	return model;
}

static void Save(Connection &con, const string &database, TableCatalogEntry &table, const Model &model) {
	// One transaction: a failure leaves the stored model as it was.
	con.BeginTransaction();
	try {
		// Never narrow: e.g. another process, which could not read the store, fitted on one
		// query's columns. A stored model with a column this one lacks stays.
		auto stored = Select(con, database, table, "column_name", "columns");
		for (idx_t i = 0; i < stored->RowCount(); i++) {
			if (!model.Find(stored->GetValue(0, i).ToString()).IsValid()) {
				con.Rollback();
				return;
			}
		}
		for (auto from : {"tables", "columns", "states", "probabilities"}) {
			StoreQuery(con, string("DELETE FROM ") + from + " WHERE database = $1 AND table_name = $2",
			           {Value(database), Value(table.name)});
		}

		Value db(database), name(table.name);
		Appender tables(con, "tables");
		tables.AppendRow(db, name, Value::BIGINT(int64_t(model.Rows())));
		tables.Close();

		Appender columns(con, "columns");
		Appender states(con, "states");
		Appender probabilities(con, "probabilities");
		auto &cols = model.Columns();
		for (auto &col : cols) {
			auto parent = col.parent.IsValid() ? Value(cols[col.parent.GetIndex()].name) : Value(LogicalType::VARCHAR);
			auto members =
			    col.members.IsValid() ? Value::BIGINT(int64_t(col.members.GetIndex())) : Value(LogicalType::BIGINT);
			columns.AppendRow(db, name, Value(col.name), parent, members);

			for (idx_t s = 0; s < col.values.size(); s++) {
				auto value = col.kinds[s] == StateKind::VALUE ? col.values[s].DefaultCastAs(LogicalType::VARCHAR)
				                                              : Value(LogicalType::VARCHAR);
				states.AppendRow(db, name, Value(col.name), Value::INTEGER(int32_t(s)), value,
				                 Value(KindName(col.kinds[s])));
			}
			for (auto &entry : col.probabilities) {
				auto parent_state =
				    col.parent.IsValid() ? Value::INTEGER(int32_t(entry.parent_state)) : Value(LogicalType::INTEGER);
				probabilities.AppendRow(db, name, Value(col.name), Value::INTEGER(int32_t(entry.state)), parent_state,
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

bool SaveModel(ClientContext &context, TableCatalogEntry &table, const Model &model) {
	auto database = DatabaseKey(table.ParentCatalog());
	if (database.empty()) {
		return false;
	}
	return WithStore(context, [&](Connection &con) { Save(con, database, table, model); });
}

vector<StoredModel> StoredModels(ClientContext &context, Catalog &catalog) {
	vector<StoredModel> models;
	auto database = DatabaseKey(catalog);
	if (database.empty()) {
		return models;
	}
	WithStore(context, [&](Connection &con) {
		auto result = StoreQuery(con,
		                         "SELECT t.table_name, t.row_count, list(c.column_name ORDER BY c.column_name) "
		                         "FROM tables t JOIN columns c USING (database, table_name) "
		                         "WHERE database = $1 GROUP BY ALL ORDER BY t.table_name",
		                         {Value(database)});
		auto &rows = *result;
		for (idx_t i = 0; i < rows.RowCount(); i++) {
			StoredModel model;
			model.table = rows.GetValue(0, i).ToString();
			model.rows = idx_t(rows.GetValue(1, i).GetValue<int64_t>());
			auto columns = rows.GetValue(2, i);
			for (auto &column : ListValue::GetChildren(columns)) {
				model.columns.push_back(column.ToString());
			}
			models.push_back(std::move(model));
		}
	});
	return models;
}

} // namespace duckdb
