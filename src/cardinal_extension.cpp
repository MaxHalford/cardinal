#define DUCKDB_EXTENSION_MAIN

#include "cardinal_extension.hpp"
#include "fit_function.hpp"
#include "inner_query.hpp"
#include "join_planner.hpp"
#include "models.hpp"
#include "store.hpp"
#include "scan_hook.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

// The extension's code: its own namespace, apart from DuckDB's names, e.g. duckdb::JoinRelation.
using namespace cardinal;

//! Runs before DuckDB's optimizers, so the hooks are in place for filter pushdown,
//! and the join targets for join ordering.
static void PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	AttachScanHooks(*plan);
	if (InInnerQuery() || CurrentEstimator(input.context) != Estimator::FANOUT) {
		return;
	}
	// Never fails the query: on any error, the query is left to DuckDB.
	try {
		SetJoinTargets(PlanJoins(input.context, input.optimizer, *plan));
	} catch (std::exception &) {
		JoinTargets hands_off;
		hands_off.hands_off = true;
		SetJoinTargets(std::move(hands_off));
	}
}

//! Runs after DuckDB's optimizers.
static void PostOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	DetachScanHooks(*plan);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(ESTIMATOR_SETTING, "Estimates given to the optimizer: fanout (scans and joins) or off",
	                          LogicalType::VARCHAR, Value("fanout"));
	config.AddExtensionOption(SAVE_SETTING, "Save fitted models in the store", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption(STORE_SETTING, "The store's file: ~/.duckdb/extension_data/cardinal/cardinal.db if empty",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption(BACKGROUND_SETTING, "Fit models on a worker thread: queries plan with what there is meanwhile",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	OptimizerExtension extension;
	extension.pre_optimize_function = PreOptimize;
	extension.optimize_function = PostOptimize;
	OptimizerExtension::Register(config, std::move(extension));

	loader.RegisterFunction(FitFunctions());
	loader.RegisterFunction(ModelsFunction());
}

void CardinalExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CardinalExtension::Name() {
	return "cardinal";
}

std::string CardinalExtension::Version() const {
#ifdef EXT_VERSION_CARDINAL
	return EXT_VERSION_CARDINAL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cardinal, loader) {
	duckdb::LoadInternal(loader);
}
}
