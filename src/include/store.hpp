//===----------------------------------------------------------------------===//
// The store: where models and keys are saved, across sessions.
//
// Its own DuckDB file, outside the user's database, as DuckDB's UI does:
//
//   ~/.duckdb/extension_data/cardinal/cardinal.db
//
// Rows are keyed by the database they describe (the absolute path of its
// file) and table. The store is opened for each use, so other processes can
// use it in between. Databases without a file, e.g. in memory, are not saved.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/connection.hpp"

#include <functional>

namespace duckdb {

//! Setting: the store's path, e.g. SET cardinal_store = '/tmp/cardinal.db'. Empty: the default.
static constexpr const char *STORE_SETTING = "cardinal_store";

//! The key of a database in the store: the absolute path of its file. Empty: it has none.
string DatabaseKey(Catalog &catalog);

//! A query on the store with parameters $1, $2, ... Throws when it fails.
unique_ptr<MaterializedQueryResult> StoreQuery(Connection &con, const string &sql, vector<Value> parameters);

//! Run fn on a connection to the store, its tables created. Uses are serialized. False when
//! the store cannot be opened, e.g. another process holds it, or when fn throws.
bool WithStore(ClientContext &context, const std::function<void(Connection &)> &fn);

} // namespace duckdb
