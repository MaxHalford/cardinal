#pragma once

#include "fit.hpp"
#include "model.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

//! Setting: fit models on a worker thread instead of while planning, e.g. SET cardinal_background = false.
static constexpr const char *BACKGROUND_SETTING = "cardinal_background";

//! Setting: save fitted models in the database, e.g. SET cardinal_save = false.
static constexpr const char *SAVE_SETTING = "cardinal_save";

//! The model of a table, fresh. In order: the one in memory, the stored one, or a new
//! fit over the columns queries filter on and the known fanouts, kept in memory. A
//! model is stale once the table's row count drifts by more than MAX_DRIFT, or when
//! it lacks one of those. Null when there is nothing to model, or fitting fails.
shared_ptr<Model> GetModel(ClientContext &context, TableCatalogEntry &table);

//! A fanout column for each known foreign key that references the table.
vector<FitFanout> KnownFanouts(ClientContext &context, TableCatalogEntry &table);

//! Replace the model of a table in memory, e.g. after cardinal_fit.
void PutModel(ClientContext &context, TableCatalogEntry &table, shared_ptr<Model> model);

} // namespace duckdb
