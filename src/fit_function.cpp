#include "fit_function.hpp"
#include "fit.hpp"
#include "inner_query.hpp"
#include "model_store.hpp"
#include "keys.hpp"
#include "models.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <chrono>
#include <thread>

namespace duckdb {


struct FitBindData : public TableFunctionData {
	vector<QualifiedName> tables;
};


//! Every table of the default database, except the models themselves.
static vector<QualifiedName> AllTables(ClientContext &context) {
	vector<QualifiedName> tables;
	auto &catalog = Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
	catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		if (schema.name == MODEL_SCHEMA) {
			return;
		}
		schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			tables.emplace_back(catalog.GetName(), schema.name, entry.name);
		});
	});
	return tables;
}

static unique_ptr<FunctionData> FitBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &types, vector<string> &names) {
	auto data = make_uniq<FitBindData>();
	if (input.inputs.empty()) {
		data->tables = AllTables(context);
	} else {
		data->tables.push_back(QualifiedName::Parse(input.inputs[0].ToString()));
	}

	names = {"table_name", "columns", "states", "seconds"};
	types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(data);
}


//! One table to fit, and what came of it.
struct FitJob {
	explicit FitJob(TableCatalogEntry &table) : table(table) {
	}

	reference<TableCatalogEntry> table;
	vector<FitColumn> columns;
	vector<FitFanout> fanouts;
	shared_ptr<Model> model;
	double seconds = 0;
	string error;
};

struct FitState : public GlobalTableFunctionState {
	bool fitted = false;
	vector<FitJob> jobs;
	//! Next job to output.
	idx_t next = 0;
};


static unique_ptr<GlobalTableFunctionState> FitInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<FitState>();
}

//! Tables fitted at once. Each fit also runs parallel queries.
static constexpr idx_t MAX_PARALLEL_FITS = 8;

//! The table's columns to model, with their distinct counts. Runs in the caller's transaction.
static FitJob Prepare(ClientContext &context, const FitBindData &data, const QualifiedName &name) {
	auto &table =
	    Catalog::GetEntry(context, CatalogType::TABLE_ENTRY, name.catalog, name.schema, name.name).Cast<TableCatalogEntry>();

	FitJob job(table);
	// Default: the columns queries filter on, when known. Tables queries only join
	// through: their fanouts alone. Else every column.
	job.fanouts = KnownFanouts(context, table);
	auto names = FilterColumns(context, table);
	if (!names.empty() || job.fanouts.empty()) {
		job.columns = FitColumns(context, table, names);
	}
	return job;
}

//! Fit jobs on worker threads, each with its own connection.
static void FitAll(DatabaseInstance &db, vector<FitJob> &jobs, const FitOptions &options) {
	atomic<idx_t> next(0);
	auto work = [&]() {
		InnerScope scope;
		Connection con(db);
		for (auto i = next++; i < jobs.size(); i = next++) {
			auto &job = jobs[i];
			auto start = std::chrono::steady_clock::now();
			try {
				job.model = FitModel(con, job.table.get(), job.columns, job.fanouts, options);
			} catch (std::exception &e) {
				job.error = e.what();
			}
			std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;
			job.seconds = seconds.count();
		}
	};

	vector<std::thread> workers;
	for (idx_t i = 0; i < MinValue<idx_t>(jobs.size(), MAX_PARALLEL_FITS); i++) {
		workers.emplace_back(work);
	}
	for (auto &worker : workers) {
		worker.join();
	}
}

static void Fit(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<FitBindData>();
	auto &state = input.global_state->Cast<FitState>();

	// First call: fit everything, write the models one table at a time.
	if (!state.fitted) {
		state.fitted = true;
		for (auto &name : data.tables) {
			state.jobs.push_back(Prepare(context, data, name));
		}
		FitAll(*context.db, state.jobs, FitOptions());

		InnerScope scope;
		Connection con(*context.db);
		for (auto &job : state.jobs) {
			if (!job.error.empty()) {
				throw InvalidInputException(job.error);
			}
			SaveModel(con, job.table.get(), *job.model);
			PutModel(context, job.table.get(), job.model);
		}
		set<string> catalogs;
		for (auto &job : state.jobs) {
			catalogs.insert(job.table.get().ParentCatalog().GetName());
		}
		for (auto &catalog : catalogs) {
			SaveKeys(con, catalog, context);
		}
	}

	idx_t row = 0;
	for (; state.next < state.jobs.size() && row < STANDARD_VECTOR_SIZE; state.next++, row++) {
		auto &job = state.jobs[state.next];
		idx_t states = 0;
		for (auto &column : job.model->Columns()) {
			states += column.values.size();
		}
		output.SetValue(0, row, Value(job.table.get().name));
		output.SetValue(1, row, Value::BIGINT(int64_t(job.columns.size())));
		output.SetValue(2, row, Value::BIGINT(int64_t(states)));
		output.SetValue(3, row, Value::DOUBLE(job.seconds));
	}
	output.SetCardinality(row);
}

TableFunctionSet FitFunctions() {
	TableFunctionSet set("cardinal_fit_eager");
	for (auto arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction function(arguments, Fit, FitBind, FitInit);
		set.AddFunction(function);
	}
	return set;
}

} // namespace duckdb
