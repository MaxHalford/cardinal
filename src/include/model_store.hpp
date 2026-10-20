#pragma once

#include "model.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

//! The schema models live in. Double underscore: managed by the extension, as DuckLake does.
static constexpr const char *MODEL_SCHEMA = "__cardinal";

//! The lock of the extension's writes to a database: one at a time, so they never conflict.
mutex &StoreLock(ClientContext &context);

//! The model of a table stored in the cardinal schema of its database. Null: none.
shared_ptr<Model> ReadModel(ClientContext &context, TableCatalogEntry &table);

//! Write a table's model to the cardinal schema of its database, replacing the old one.
void SaveModel(Connection &con, TableCatalogEntry &table, const Model &model);

} // namespace duckdb
