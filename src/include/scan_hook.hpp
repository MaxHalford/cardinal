#pragma once

#include "join_planner.hpp"

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {
namespace cardinal {

//! Which estimates the hooks give DuckDB, from the cardinal_estimator setting.
//!   EXACT    scans: true counts, a test oracle
//!   BN       scans: table models
//!   FANOUT   scans and joins: table models with fanouts, fitted to DuckDB's join formula
enum class Estimator { OFF, EXACT, BN, FANOUT };

//! Setting that picks the estimator, e.g. SET cardinal_estimator = 'exact'.
static constexpr const char *ESTIMATOR_SETTING = "cardinal_estimator";

Estimator CurrentEstimator(ClientContext &context);

//! Start an optimizer run: attach the hooks to every DuckDB table scan in the plan.
void AttachScanHooks(LogicalOperator &op);

//! Join targets of the current optimizer run: row and distinct counts the hooks report.
void SetJoinTargets(JoinTargets targets);

//! Start a run on a copy of a plan, after filter pushdown: the scans report the targets.
//! e.g. to see the join order DuckDB picks with them. End it with DetachScanHooks.
void AttachTargets(ClientContext &context, LogicalOperator &plan, const JoinTargets &targets);

//! End the optimizer run started by the last AttachScanHooks or AttachTargets.
void DetachScanHooks();

} // namespace cardinal
} // namespace duckdb
