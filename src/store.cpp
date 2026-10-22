#include "store.hpp"
#include "inner_query.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <chrono>
#include <thread>

namespace duckdb {
namespace cardinal {

//! Version of the store's tables. A change starts a new store, as a DuckDB release does.
static constexpr idx_t STORE_VERSION = 1;

static const char *STORE_TABLES = R"(
CREATE TABLE IF NOT EXISTS tables (database VARCHAR, table_name VARCHAR, row_count BIGINT);
CREATE TABLE IF NOT EXISTS columns (database VARCHAR, table_name VARCHAR, column_name VARCHAR, parent VARCHAR, members BIGINT);
CREATE TABLE IF NOT EXISTS states (database VARCHAR, table_name VARCHAR, column_name VARCHAR, state INTEGER, value VARCHAR, kind VARCHAR);
CREATE TABLE IF NOT EXISTS probabilities (database VARCHAR, table_name VARCHAR, column_name VARCHAR, state INTEGER, parent_state INTEGER, probability DOUBLE);
CREATE TABLE IF NOT EXISTS keys (database VARCHAR, fk_schema VARCHAR, fk_table VARCHAR, fk_column VARCHAR, pk_schema VARCHAR, pk_table VARCHAR, pk_column VARCHAR);
)";

//! One use of the store at a time, in this process.
static mutex store_lock;

//! Attempts to open the store when another process holds it, and the wait between them.
static constexpr idx_t OPEN_ATTEMPTS = 5;
static constexpr idx_t OPEN_WAIT_MS = 25;

string DatabaseKey(Catalog &catalog) {
	if (!catalog.IsDuckCatalog() || catalog.InMemory()) {
		return "";
	}
	auto &attached = catalog.GetAttached();
	if (attached.IsTemporary() || attached.IsSystem()) {
		return "";
	}
	auto path = attached.GetStorageManager().GetDBPath();
	if (path.empty()) {
		return "";
	}
	LocalFileSystem fs;
	return fs.IsPathAbsolute(path) ? path : fs.JoinPath(FileSystem::GetWorkingDirectory(), path);
}

static string StorePath(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(STORE_SETTING, value) && !value.IsNull() && !value.ToString().empty()) {
		return value.ToString();
	}
	LocalFileSystem fs;
	auto home = FileSystem::GetHomeDirectory(nullptr);
	// One store per DuckDB release: no migration, models are fitted again after an upgrade.
	// e.g. ~/.duckdb/extension_data/cardinal/v2.0.0/store-1.db
	auto file = "store-" + to_string(STORE_VERSION) + ".db";
	return fs.JoinPath(home, ".duckdb", "extension_data", "cardinal", DuckDB::LibraryVersion(), file);
}

unique_ptr<QueryRows> StoreQuery(Connection &con, const string &sql, vector<Value> parameters) {
	auto statement = con.Prepare(sql);
	if (statement->HasError()) {
		throw InvalidInputException("cardinal: %s", statement->GetError());
	}
	auto result = statement->Execute(parameters);
	if (result->HasError()) {
		throw InvalidInputException("cardinal: %s", result->GetError());
	}
	return make_uniq<QueryRows>(*result);
}

bool WithStore(ClientContext &context, const std::function<void(Connection &)> &fn) {
	auto path = StorePath(context);
	lock_guard<mutex> guard(store_lock);
	// The store's own queries are planned too: the extension leaves them alone.
	InnerScope scope;
	try {
		LocalFileSystem fs;
		auto directory = path.substr(0, path.find_last_of("/\\"));
		if (!directory.empty() && directory != path && !fs.DirectoryExists(directory)) {
			fs.CreateDirectoriesRecursive(directory);
		}
		DBConfig config;
		config.options.maximum_threads = 1;
		unique_ptr<DuckDB> store;
		for (idx_t attempt = 0; !store; attempt++) {
			try {
				store = make_uniq<DuckDB>(path, &config);
			} catch (std::exception &) {
				if (attempt + 1 == OPEN_ATTEMPTS) {
					throw;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(OPEN_WAIT_MS));
			}
		}
		Connection con(*store);
		auto created = con.Query(STORE_TABLES);
		if (created->HasError()) {
			return false;
		}
		fn(con);
		return true;
	} catch (std::exception &) {
		return false;
	}
}

} // namespace cardinal
} // namespace duckdb
