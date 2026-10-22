//===----------------------------------------------------------------------===//
// Models in the store (store.hpp), one row set per database and table:
//
//   tables          database, schema_name, table_name, row_count
//   columns         database, schema_name, table_name, column_name, parent, members
//   states          database, schema_name, table_name, column_name, state, value, kind
//   probabilities   database, schema_name, table_name, column_name, state, parent_state, probability
//
// Values are text: they are cast to the table column's type when read.
//===----------------------------------------------------------------------===//

#include "model_store.hpp"
#include "store.hpp"

#include "duckdb/main/appender.hpp"

namespace duckdb {
namespace cardinal {

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

//! e.g. SELECT ... FROM states WHERE database = $1 AND schema_name = $2 AND table_name = $3 ORDER BY ...
static unique_ptr<QueryRows> Select(Connection &con, const string &database, TableCatalogEntry &table,
                                    const string &columns, const string &from, const string &order = "") {
	return StoreQuery(con,
	                  "SELECT " + columns + " FROM " + from +
	                      " WHERE database = $1 AND schema_name = $2 AND table_name = $3" +
	                      (order.empty() ? "" : " ORDER BY " + order),
	                  {Value(database), Value(table.ParentSchema().name), Value(table.name)});
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
		column.type = table_columns.ColumnExists(Identifier(column.name))
		                  ? table_columns.GetColumn(Identifier(column.name)).Type()
		                  : LogicalType::DOUBLE;
	}

	// States arrive in order: state i is the i-th of its column.
	auto states_result =
	    Select(con, database, table, "column_name, state, value, kind", "states", "column_name, state");
	auto &states = *states_result;
	for (idx_t i = 0; i < states.RowCount(); i++) {
		auto &column = columns[index.at(states.GetValue(0, i).ToString())];
		auto kind = ParseKind(states.GetValue(3, i).ToString());
		// Stored as text: back to the column's type.
		Value value = states.GetValue(2, i);
		if (kind == StateKind::VALUE) {
			auto cast = value.DefaultTryCastAs(column.type);
			if (!cast) {
				return nullptr;
			}
			value = *cast;
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
			StoreQuery(con,
			           string("DELETE FROM ") + from + " WHERE database = $1 AND schema_name = $2 AND table_name = $3",
			           {Value(database), Value(table.ParentSchema().name), Value(table.name)});
		}

		Value db(database), schema(table.ParentSchema().name), name(table.name);
		Appender tables(con, "tables");
		tables.AppendRow(db, schema, name, Value::BIGINT(int64_t(model.Rows())));
		tables.Close();

		Appender columns(con, "columns");
		Appender states(con, "states");
		Appender probabilities(con, "probabilities");
		auto &cols = model.Columns();
		for (auto &col : cols) {
			auto parent = col.parent.IsValid() ? Value(cols[col.parent.GetIndex()].name) : Value(LogicalType::VARCHAR);
			auto members =
			    col.members.IsValid() ? Value::BIGINT(int64_t(col.members.GetIndex())) : Value(LogicalType::BIGINT);
			columns.AppendRow(db, schema, name, Value(col.name), parent, members);

			for (idx_t s = 0; s < col.values.size(); s++) {
				auto value = col.kinds[s] == StateKind::VALUE ? col.values[s].DefaultCastAs(LogicalType::VARCHAR)
				                                              : Value(LogicalType::VARCHAR);
				states.AppendRow(db, schema, name, Value(col.name), Value::INTEGER(int32_t(s)), value,
				                 Value(KindName(col.kinds[s])));
			}
			for (auto &entry : col.probabilities) {
				auto parent_state =
				    col.parent.IsValid() ? Value::INTEGER(int32_t(entry.parent_state)) : Value(LogicalType::INTEGER);
				probabilities.AppendRow(db, schema, name, Value(col.name), Value::INTEGER(int32_t(entry.state)),
				                        parent_state, Value::DOUBLE(entry.p));
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
	// Plain SQL, grouped here: the store's instance may lack core functions, e.g. list().
	WithStore(context, [&](Connection &con) {
		auto result = StoreQuery(con,
		                         "SELECT t.schema_name, t.table_name, t.row_count, c.column_name "
		                         "FROM tables t JOIN columns c USING (database, schema_name, table_name) "
		                         "WHERE database = $1 ORDER BY t.schema_name, t.table_name, c.column_name",
		                         {Value(database)});
		auto &rows = *result;
		for (idx_t i = 0; i < rows.RowCount(); i++) {
			auto schema = rows.GetValue(0, i).ToString();
			auto table = rows.GetValue(1, i).ToString();
			if (models.empty() || models.back().schema != schema || models.back().table != table) {
				StoredModel model;
				model.schema = schema;
				model.table = table;
				model.rows = idx_t(rows.GetValue(2, i).GetValue<int64_t>());
				models.push_back(std::move(model));
			}
			models.back().columns.push_back(rows.GetValue(3, i).ToString());
		}
	});
	return models;
}

} // namespace cardinal
} // namespace duckdb
