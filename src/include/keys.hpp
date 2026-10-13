//===----------------------------------------------------------------------===//
// What queries teach about a database: the columns they filter on, and the
// primary and foreign keys they join. Models are fitted on these.
//
// Primary and foreign keys are learned from the data and the queries.
//
// A column is a primary key when its values are unique: declared, or checked.
// A foreign key references the primary key it is joined to. Pairs are
// remembered, so a query joining two foreign keys without their primary key
// still knows the table they point to:
//
//     cast_info.movie_id ──► title.id ◄── movie_info.movie_id
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

//! A table column, by name: "catalog"."schema"."table" and column.
struct KeyColumn {
	string catalog;
	string schema;
	string table;
	string column;

	KeyColumn() = default;
	KeyColumn(TableCatalogEntry &table, string column);
	string ToString() const;
	bool operator<(const KeyColumn &other) const;
	bool operator==(const KeyColumn &other) const;
};

//! Whether the column's non-null values are unique. Cached per row count.
bool IsUnique(ClientContext &context, TableCatalogEntry &table, const string &column);

//! Remember that fk references pk.
void AddForeignKey(ClientContext &context, const KeyColumn &fk, const KeyColumn &pk);

//! The primary key fk references, if known.
bool ReferencedKey(ClientContext &context, const KeyColumn &fk, KeyColumn &pk);

//! The known foreign keys that reference a column of the table, with the column: (fk, pk column).
vector<pair<KeyColumn, string>> ReferencingKeys(ClientContext &context, TableCatalogEntry &table);

//! Remember that queries filter on these columns of the table.
void AddFilterColumns(ClientContext &context, TableCatalogEntry &table, const vector<string> &columns);

//! The columns queries filter on, sorted.
vector<string> FilterColumns(ClientContext &context, TableCatalogEntry &table);

//! Write the known foreign keys of a catalog to its cardinal.keys table, replacing them.
//! They are read back the first time the catalog is used.
void SaveKeys(Connection &con, const string &catalog, ClientContext &context);

//! The table of a key column, if it still exists.
optional_ptr<TableCatalogEntry> KeyTable(ClientContext &context, const KeyColumn &column);

} // namespace duckdb
