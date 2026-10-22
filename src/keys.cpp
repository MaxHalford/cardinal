#include "keys.hpp"
#include "inner_query.hpp"
#include "store.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

//! Below this share of distinct values per row, a column is not unique: no need to check.
static constexpr double UNIQUE_HINT = 0.5;

KeyColumn::KeyColumn(TableCatalogEntry &table, string column_p)
    : catalog(table.ParentCatalog().GetName()), schema(table.ParentSchema().name), table(table.name),
      column(std::move(column_p)) {
}

string KeyColumn::ToString() const {
	return catalog + "." + schema + "." + table + "." + column;
}

bool KeyColumn::operator<(const KeyColumn &other) const {
	return ToString() < other.ToString();
}

bool KeyColumn::operator==(const KeyColumn &other) const {
	return ToString() == other.ToString();
}

//! What is known about keys in a database, in DuckDB's object cache.
struct KeyRegistry : public ObjectCacheEntry {
	static string ObjectType() {
		return "cardinal_keys";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	mutex lock;
	//! Uniqueness by column, with the row count it was checked at.
	map<KeyColumn, pair<idx_t, bool>> unique;
	//! fk -> pk
	map<KeyColumn, KeyColumn> references;
	//! Filtered columns, by table: "catalog.schema.table."
	map<string, set<string>> filtered;
	//! Catalogs whose stored keys were read.
	set<string> loaded;
};

//! Stored foreign keys: the store's keys table, by database.
static constexpr const char *KEYS_COLUMNS = "fk_schema, fk_table, fk_column, pk_schema, pk_table, pk_column";

//! The registry, with the stored keys of the catalog read once.
static KeyRegistry &Registry(ClientContext &context, const string &catalog) {
	auto &registry = *ObjectCache::GetObjectCache(context).GetOrCreate<KeyRegistry>("cardinal:keys");
	{
		lock_guard<mutex> guard(registry.lock);
		if (registry.loaded.count(catalog) || InInnerQuery()) {
			return registry;
		}
		registry.loaded.insert(catalog);
	}
	string database;
	try {
		database = DatabaseKey(Catalog::GetCatalog(context, Identifier(catalog)));
	} catch (std::exception &) {
	}
	if (database.empty()) {
		return registry;
	}
	vector<pair<KeyColumn, KeyColumn>> stored;
	WithStore(context, [&](Connection &con) {
		auto result = StoreQuery(con, string("SELECT ") + KEYS_COLUMNS + " FROM keys WHERE database = $1",
		                         {Value(database)});
		auto &rows = *result;
		for (idx_t i = 0; i < rows.RowCount(); i++) {
			KeyColumn fk, pk;
			fk.catalog = pk.catalog = catalog;
			fk.schema = rows.GetValue(0, i).ToString();
			fk.table = rows.GetValue(1, i).ToString();
			fk.column = rows.GetValue(2, i).ToString();
			pk.schema = rows.GetValue(3, i).ToString();
			pk.table = rows.GetValue(4, i).ToString();
			pk.column = rows.GetValue(5, i).ToString();
			stored.emplace_back(fk, pk);
		}
	});
	lock_guard<mutex> guard(registry.lock);
	for (auto &key : stored) {
		registry.references.emplace(key.first, key.second);
	}
	return registry;
}

static idx_t Rows(TableCatalogEntry &table) {
	return table.IsDuckTable() ? table.GetStorage().GetTotalRows() : 0;
}

static bool DeclaredUnique(TableCatalogEntry &table, const string &column) {
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		auto &names = unique.GetColumnNames();
		if (names.size() == 1 && names[0] == Identifier(column)) {
			return true;
		}
		if (names.empty() && unique.HasIndex() &&
		    table.GetColumn(unique.GetIndex()).Name() == Identifier(column)) {
			return true;
		}
	}
	return false;
}

//! Checks with a query: count(DISTINCT c) = count(c).
static bool CheckUnique(ClientContext &context, TableCatalogEntry &table, const string &column) {
	auto &definition = table.GetColumns().GetColumn(Identifier(column));
	auto stats = table.GetStatistics(context, definition.Logical().index);
	auto rows = Rows(table);
	if (stats && rows > 0 && double(stats->GetDistinctCount()) < UNIQUE_HINT * double(rows)) {
		return false;
	}
	auto name = KeywordHelper::WriteOptionallyQuoted(column);
	auto result = InnerQuery(context, "SELECT count(DISTINCT " + name + ") = count(" + name + ") AND count(" + name +
	                                      ") > 0 FROM " + KeywordHelper::WriteOptionallyQuoted(table.ParentCatalog().GetName().GetIdentifierName()) +
	                                      "." + KeywordHelper::WriteOptionallyQuoted(table.ParentSchema().name.GetIdentifierName()) + "." +
	                                      KeywordHelper::WriteOptionallyQuoted(table.name.GetIdentifierName()));
	return result && result->GetValue(0, 0).GetValue<bool>();
}

bool IsUnique(ClientContext &context, TableCatalogEntry &table, const string &column) {
	if (DeclaredUnique(table, column)) {
		return true;
	}
	auto &registry = Registry(context, table.ParentCatalog().GetName().GetIdentifierName());
	KeyColumn key(table, column);
	auto rows = Rows(table);
	{
		lock_guard<mutex> guard(registry.lock);
		auto hit = registry.unique.find(key);
		if (hit != registry.unique.end() && hit->second.first == rows) {
			return hit->second.second;
		}
	}
	auto unique = CheckUnique(context, table, column);
	lock_guard<mutex> guard(registry.lock);
	registry.unique[key] = make_pair(rows, unique);
	return unique;
}

void AddForeignKey(ClientContext &context, const KeyColumn &fk, const KeyColumn &pk) {
	auto &registry = Registry(context, fk.catalog);
	lock_guard<mutex> guard(registry.lock);
	registry.references[fk] = pk;
}

bool ReferencedKey(ClientContext &context, const KeyColumn &fk, KeyColumn &pk) {
	auto &registry = Registry(context, fk.catalog);
	lock_guard<mutex> guard(registry.lock);
	auto hit = registry.references.find(fk);
	if (hit == registry.references.end()) {
		return false;
	}
	pk = hit->second;
	return true;
}

vector<pair<KeyColumn, string>> ReferencingKeys(ClientContext &context, TableCatalogEntry &table) {
	auto &registry = Registry(context, table.ParentCatalog().GetName().GetIdentifierName());
	KeyColumn self(table, "");
	vector<pair<KeyColumn, string>> result;
	lock_guard<mutex> guard(registry.lock);
	for (auto &entry : registry.references) {
		auto &pk = entry.second;
		if (pk.catalog == self.catalog && pk.schema == self.schema && pk.table == self.table) {
			result.emplace_back(entry.first, pk.column);
		}
	}
	return result;
}

void AddFilterColumns(ClientContext &context, TableCatalogEntry &table, const vector<string> &columns) {
	auto &registry = Registry(context, table.ParentCatalog().GetName().GetIdentifierName());
	lock_guard<mutex> guard(registry.lock);
	auto &known = registry.filtered[KeyColumn(table, "").ToString()];
	known.insert(columns.begin(), columns.end());
}

vector<string> FilterColumns(ClientContext &context, TableCatalogEntry &table) {
	auto &registry = Registry(context, table.ParentCatalog().GetName().GetIdentifierName());
	lock_guard<mutex> guard(registry.lock);
	auto &known = registry.filtered[KeyColumn(table, "").ToString()];
	return vector<string>(known.begin(), known.end());
}

bool SaveKeys(ClientContext &context, Catalog &catalog) {
	auto database = DatabaseKey(catalog);
	if (database.empty()) {
		return false;
	}
	auto &registry = Registry(context, catalog.GetName().GetIdentifierName());
	vector<pair<KeyColumn, KeyColumn>> keys;
	{
		lock_guard<mutex> guard(registry.lock);
		for (auto &entry : registry.references) {
			if (entry.first.catalog == catalog.GetName()) {
				keys.emplace_back(entry.first, entry.second);
			}
		}
	}

	// In one transaction: the keys of the database are replaced at once.
	return WithStore(context, [&](Connection &con) {
		con.BeginTransaction();
		try {
			StoreQuery(con, "DELETE FROM keys WHERE database = $1", {Value(database)});
			Appender appender(con, "keys");
			for (auto &key : keys) {
				appender.AppendRow(Value(database), Value(key.first.schema), Value(key.first.table),
				                   Value(key.first.column), Value(key.second.schema), Value(key.second.table),
				                   Value(key.second.column));
			}
			appender.Close();
			con.Commit();
		} catch (std::exception &) {
			con.Rollback();
			throw;
		}
	});
}

optional_ptr<TableCatalogEntry> KeyTable(ClientContext &context, const KeyColumn &column) {
	QualifiedName name(Identifier(column.catalog), Identifier(column.schema), Identifier(column.table));
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, name);
	auto entry = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return nullptr;
	}
	return &entry->Cast<TableCatalogEntry>();
}

} // namespace duckdb
