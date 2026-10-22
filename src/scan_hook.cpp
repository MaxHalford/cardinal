//===----------------------------------------------------------------------===//
// Scan hook: give DuckDB's join order optimizer our base table estimates.
//
// DuckDB has no hook for cardinality estimation. But the join order
// optimizer reads a scan's row count from LogicalGet::estimated_cardinality
// when it is set, then applies its own filter rules on top:
//
//   rows / distinct values   for an equality filter
//   x 0.2                    for any other filter in the scan
//   x 0.2                    for a filter left above the scan
//
// Filter pushdown calls pushdown_complex_filter on each scan with the
// scan's filters. DuckDB's table scan does not use it, so we do:
//
//   pre-optimize        attach the hooks to each table scan
//   filter pushdown     hook: estimate, divide by DuckDB's filter factor,
//                       remember as the scan's base row count
//   join order          asks the scan's cardinality callback: the base
//                       row count. Applies its filter factor: our estimate.
//                       Sets the scan's estimate, which ends the hook's work.
//   statistics          pushes derived filters again: the hook ignores them
//
// With join targets, the scan reports the fitted row count instead of its
// estimate, and its join columns the fitted distinct counts.
//===----------------------------------------------------------------------===//

#include "scan_hook.hpp"
#include "bn_estimator.hpp"
#include "exact.hpp"
#include "inner_query.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {
namespace cardinal {

//! Base row counts of the scans of one optimizer run, by bind data.
//! A stack: the exact estimator plans its count queries inside a run.
struct Frame {
	unordered_map<const FunctionData *, idx_t> rows;
	unordered_map<const FunctionData *, table_function_cardinality_t> original;
	unordered_map<const FunctionData *, table_statistics_extended_t> original_statistics;
	unordered_map<const FunctionData *, TableIndex> table_index;
	JoinTargets targets;
};
static thread_local vector<Frame> frames;

//! Row count of the probe scan that measures DuckDB's filter factor.
//! Large, so integer rounding does not distort the factor.
static constexpr idx_t PROBE_ROWS = 1000000000000;

Estimator CurrentEstimator(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting(ESTIMATOR_SETTING, value)) {
		return Estimator::FANOUT;
	}
	auto name = StringUtil::Lower(value.ToString());
	if (name == "exact") {
		return Estimator::EXACT;
	}
	if (name == "bn") {
		return Estimator::BN;
	}
	if (name == "fanout") {
		return Estimator::FANOUT;
	}
	return Estimator::OFF;
}

//! cardinality of the probe scan.
static unique_ptr<NodeStatistics> ProbeCardinality(ClientContext &context, const FunctionData *bind_data) {
	return make_uniq<NodeStatistics>(PROBE_ROWS);
}

//! Share of a scan's rows DuckDB keeps after these filters, by its own rules.
//! Runs DuckDB's filter pushdown and scan statistics on a copy of the scan.
//! Returns 0 when the filters are unsatisfiable.
static double FilterFactor(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	// The copy comes from the catalog, without the hook.
	auto copy = get.Copy(context);
	auto &probe = copy->Cast<LogicalGet>();
	probe.function.cardinality = ProbeCardinality;
	probe.function.pushdown_complex_filter = nullptr;

	unique_ptr<LogicalOperator> root = std::move(copy);
	if (!filters.empty()) {
		auto filter = make_uniq<LogicalFilter>();
		for (auto &expr : filters) {
			filter->expressions.push_back(expr->Copy());
		}
		filter->children.push_back(std::move(root));
		root = std::move(filter);
	}

	auto binder = Binder::CreateBinder(context);
	Optimizer optimizer(*binder, context);
	FilterPushdown pushdown(optimizer);
	root = pushdown.Rewrite(std::move(root));

	// Filters that could not go into the scan stay above it.
	bool filter_above = root->type == LogicalOperatorType::LOGICAL_FILTER;
	auto op = root.get();
	while (op->type != LogicalOperatorType::LOGICAL_GET) {
		if (op->children.empty()) {
			return 0;
		}
		op = op->children[0].get();
	}

	auto stats = RelationStatisticsHelper::ExtractGetStats(op->Cast<LogicalGet>(), context);
	auto rows = double(stats.cardinality);
	if (filter_above) {
		rows = MaxValue(rows * RelationStatisticsHelper::DEFAULT_SELECTIVITY, 1.0);
	}
	return rows / double(PROBE_ROWS);
}

//! The scan's own row count, before filters.
static optional_idx TableRows(ClientContext &context, const FunctionData *bind_data) {
	auto &original = frames.back().original;
	auto hit = original.find(bind_data);
	if (hit == original.end() || !hit->second) {
		return optional_idx();
	}
	auto stats = hit->second(context, bind_data);
	if (!stats || !stats->has_estimated_cardinality) {
		return optional_idx();
	}
	return stats->estimated_cardinality;
}

static optional_idx Estimate(Estimator estimator, ClientContext &context, LogicalGet &get,
                             const vector<unique_ptr<Expression>> &filters) {
	switch (estimator) {
	case Estimator::EXACT:
		return ExactCount(context, get, filters);
	case Estimator::BN:
	case Estimator::FANOUT: {
		auto &targets = frames.back().targets.rows;
		auto target = targets.find(get.table_index);
		if (target != targets.end()) {
			return idx_t(std::round(target->second));
		}
		auto rows = TableRows(context, get.bind_data.get());
		if (!rows.IsValid()) {
			return optional_idx();
		}
		return BNEstimate(context, get, filters, rows.GetIndex());
	}
	default:
		return optional_idx();
	}
}

//! The scan hook's work. May throw.
static void EstimateScan(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                         vector<unique_ptr<Expression>> &filters) {
	// Join order set the estimate: later pushdowns are not ours to change.
	if (get.has_estimated_cardinality || frames.empty() || frames.back().targets.hands_off) {
		return;
	}
	auto estimator = CurrentEstimator(context);
	if (estimator == Estimator::OFF) {
		return;
	}

	auto rows = Estimate(estimator, context, get, filters);
	if (!rows.IsValid()) {
		return;
	}
	auto factor = FilterFactor(context, get, filters);
	if (factor <= 0) {
		return;
	}

	// e.g. 1,000 estimated rows behind one range filter: 5,000, which DuckDB x 0.2 = 1,000.
	auto base = std::round(double(rows.GetIndex()) / factor);
	frames.back().rows[bind_data] = MaxValue<idx_t>(idx_t(base), 1);
}

//! pushdown_complex_filter: the filters stay as they are, only the row count changes.
//! Never fails the query: on any error, the scan keeps DuckDB's estimate.
static void ScanHook(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                     vector<unique_ptr<Expression>> &filters) {
	try {
		EstimateScan(context, get, bind_data, filters);
	} catch (std::exception &) {
		if (!frames.empty()) {
			frames.back().rows.erase(bind_data);
		}
	}
}

//! cardinality: the remembered base row count, else the scan's own.
static unique_ptr<NodeStatistics> ScanCardinality(ClientContext &context, const FunctionData *bind_data) {
	for (auto frame = frames.rbegin(); frame != frames.rend(); frame++) {
		auto hit = frame->rows.find(bind_data);
		if (hit != frame->rows.end()) {
			return make_uniq<NodeStatistics>(hit->second);
		}
		auto original = frame->original.find(bind_data);
		if (original != frame->original.end()) {
			return original->second ? original->second(context, bind_data) : nullptr;
		}
	}
	return nullptr;
}

//! statistics_extended: the fitted distinct count of a join column, else the scan's own.
static unique_ptr<BaseStatistics> ScanStatistics(ClientContext &context, TableFunctionGetStatisticsInput &input) {
	auto bind_data = input.bind_data.get();
	for (auto frame = frames.rbegin(); frame != frames.rend(); frame++) {
		auto original = frame->original_statistics.find(bind_data);
		if (original == frame->original_statistics.end()) {
			continue;
		}
		auto stats = original->second(context, input);
		if (!stats || !input.column_index.HasPrimaryIndex()) {
			return stats;
		}
		auto key = make_pair(frame->table_index[bind_data], input.column_index.GetPrimaryIndex());
		auto target = frame->targets.distinct.find(key);
		if (stats && target != frame->targets.distinct.end()) {
			stats->SetDistinctCount(target->second);
		}
		return stats;
	}
	return nullptr;
}

static void AttachHooks(LogicalOperator &op) {
	for (auto &child : op.children) {
		AttachHooks(*child);
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}

	// Only scans of DuckDB tables, which leave the hook free.
	auto &get = op.Cast<LogicalGet>();
	if (!get.GetTable() || get.function.pushdown_complex_filter || !get.bind_data) {
		return;
	}
	auto &frame = frames.back();
	frame.original[get.bind_data.get()] = get.function.cardinality;
	frame.table_index[get.bind_data.get()] = get.table_index;
	get.function.pushdown_complex_filter = ScanHook;
	get.function.cardinality = ScanCardinality;
	if (get.function.statistics_extended) {
		frame.original_statistics[get.bind_data.get()] = get.function.statistics_extended;
		get.function.statistics_extended = ScanStatistics;
	}
}

void AttachScanHooks(LogicalOperator &op) {
	frames.emplace_back();
	// Inner queries keep DuckDB's estimates: an empty frame, no hooks.
	if (InInnerQuery()) {
		return;
	}
	AttachHooks(op);
}

//! Hooks on the scans of an analyzed copy: each scan's base row count is its target over
//! DuckDB's filter factor, given the filters left above it.
static void AttachTargetHooks(ClientContext &context, LogicalOperator &op,
                              const vector<unique_ptr<Expression>> *above) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER && op.children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		AttachTargetHooks(context, *op.children[0], &op.expressions);
		return;
	}
	for (auto &child : op.children) {
		AttachTargetHooks(context, *child, nullptr);
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = op.Cast<LogicalGet>();
	if (!get.GetTable() || !get.bind_data) {
		return;
	}
	AttachHooks(op);
	auto &frame = frames.back();
	auto target = frame.targets.rows.find(get.table_index);
	if (target == frame.targets.rows.end()) {
		return;
	}
	vector<unique_ptr<Expression>> none;
	auto factor = FilterFactor(context, get, above ? *above : none);
	if (factor > 0) {
		frame.rows[get.bind_data.get()] = MaxValue<idx_t>(idx_t(std::round(target->second / factor)), 1);
	}
}

void AttachTargets(ClientContext &context, LogicalOperator &plan, const JoinTargets &targets) {
	frames.emplace_back();
	frames.back().targets = targets;
	AttachTargetHooks(context, plan, nullptr);
}

void SetJoinTargets(JoinTargets targets) {
	if (!frames.empty()) {
		frames.back().targets = std::move(targets);
	}
}

void DetachScanHooks() {
	// An optimizer run that throws leaves its frame: harmless, it is never on top again.
	if (!frames.empty()) {
		frames.pop_back();
	}
}

} // namespace cardinal
} // namespace duckdb
