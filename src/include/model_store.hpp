#pragma once

#include "model.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {
namespace cardinal {

//! The model of a table saved in the store. Null: none, or the store is unavailable.
shared_ptr<Model> ReadModel(ClientContext &context, TableCatalogEntry &table);

//! Save a table's model in the store, replacing the old one. False when it was not saved.
bool SaveModel(ClientContext &context, TableCatalogEntry &table, const Model &model);

//! A model saved in the store: its table, rows at fit time, and columns.
struct StoredModel {
	string schema;
	string table;
	idx_t rows;
	vector<string> columns;
};

//! The models saved for a database.
vector<StoredModel> StoredModels(ClientContext &context, Catalog &catalog);

} // namespace cardinal
} // namespace duckdb
