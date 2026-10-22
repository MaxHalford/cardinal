#pragma once

#include "model.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace cardinal {

struct FitOptions {
	//! States of a compacted column, OTHER included.
	idx_t max_categories = 500;
	//! Share of non-null rows the kept values cover, at least.
	double coverage = 0.9;
	//! Rows the model learns from. 0: every row.
	idx_t sample_size = 1000000;
};

struct FitColumn {
	string name;
	//! Distinct values in the whole table, e.g. from DuckDB's statistics. Invalid: unknown.
	optional_idx distinct;
};

//! A fanout column to fit: per row of the table, the rows of fk_table whose fk_column
//! equals its pk column.
struct FitFanout {
	string pk;
	reference<TableCatalogEntry> fk_table;
	string fk_column;
};

//! Whether columns of the type can be modeled: flat values only, no lists, structs or blobs.
bool SupportedType(const LogicalType &type);

//! The given columns of a table, every supported one when none are given, with their
//! distinct counts from DuckDB's statistics. Throws when there is none to model.
vector<FitColumn> FitColumns(ClientContext &context, TableCatalogEntry &table, const vector<string> &names);

//! Fit a table model over the given columns, with queries on con.
//! The first column is the root of the tree.
shared_ptr<Model> FitModel(Connection &con, TableCatalogEntry &table, const vector<FitColumn> &columns,
                           const vector<FitFanout> &fanouts, const FitOptions &options);

} // namespace cardinal
} // namespace duckdb
