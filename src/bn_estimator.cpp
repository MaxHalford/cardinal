//===----------------------------------------------------------------------===//
// Filters -> P(filters), with a table model.
//
//   1. Column references point to model columns.
//   2. Each filter becomes ORed conjunctions of single-column predicates
//      (DNF), e.g. a = 1 OR (a = 2 AND b < 3) -> [{a: a = 1}, {a: a = 2, b: b < 3}]
//   3. Each conjunction gives weights: DuckDB evaluates a column's predicates
//      on the column's states. OTHER, the infrequent values grouped
//      together, gets a share:
//        =, IN, !=        each infrequent value is equally frequent:
//                         share = matching infrequent values / members
//        anything else    as many as among frequent values, smoothed:
//                         share = (matches + 1) / (frequent values + 2)
//   4. P(A or B) = P(A) + P(B) - P(A and B): inclusion-exclusion.
//
// Same semantics as sorobn, which fits the models.
//===----------------------------------------------------------------------===//

#include "bn_estimator.hpp"
#include "fit.hpp"
#include "keys.hpp"
#include "models.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {
namespace cardinal {

//! Inclusion-exclusion over more terms than this is too slow.
static constexpr idx_t MAX_TERMS = 10;

//! Predicates by model column, ANDed. Pointers into the normalized filters.
using Term = map<idx_t, vector<const Expression *>>;

//! A model column's place in the scan: its type.
struct Target {
	idx_t column;
	LogicalType type;
};

//! Filter expression with column references replaced by model column references.
//! Null when a column is not in the model.
static unique_ptr<Expression> Normalize(const Expression &expr, LogicalGet &get, const Model &model) {
	auto copy = expr.Copy();
	bool ok = true;
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(copy, [&](BoundColumnRefExpression &ref,
	                                                                               unique_ptr<Expression> &slot) {
		auto &ids = get.GetColumnIds();
		if (ref.Binding().table_index != get.table_index || ref.Binding().column_index >= ids.size()) {
			ok = false;
			return;
		}
		auto primary = ids[ref.Binding().column_index].GetPrimaryIndex();
		auto column = primary < get.names.size() ? model.Find(get.names[primary].GetIdentifierName()) : optional_idx();
		if (!column.IsValid()) {
			ok = false;
			return;
		}
		slot = make_uniq<BoundReferenceExpression>(ref.GetReturnType(), column.GetIndex());
	});
	return ok ? std::move(copy) : nullptr;
}

static set<idx_t> Columns(const Expression &expr) {
	set<idx_t> columns;
	ExpressionIterator::VisitExpression<BoundReferenceExpression>(
	    expr, [&](const BoundReferenceExpression &ref) { columns.insert(ref.Index()); });
	return columns;
}

//! a AND b: every pair of terms, merged.
static vector<Term> Product(const vector<Term> &left, const vector<Term> &right) {
	vector<Term> result;
	for (auto &a : left) {
		for (auto &b : right) {
			auto term = a;
			for (auto &entry : b) {
				auto &preds = term[entry.first];
				preds.insert(preds.end(), entry.second.begin(), entry.second.end());
			}
			result.push_back(std::move(term));
		}
	}
	return result;
}

//! Expression -> ORed terms. False when it mixes columns other than through AND and OR.
static bool DNF(const Expression &expr, vector<Term> &result) {
	auto columns = Columns(expr);
	if (columns.size() == 1) {
		result = {Term {{*columns.begin(), {&expr}}}};
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}

	auto &conjunction = expr.Cast<BoundConjunctionExpression>();
	bool is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
	result = is_and ? vector<Term> {Term()} : vector<Term>();
	for (auto &child : conjunction.GetChildren()) {
		vector<Term> terms;
		if (!DNF(*child, terms)) {
			return false;
		}
		if (is_and) {
			result = Product(result, terms);
		} else {
			result.insert(result.end(), terms.begin(), terms.end());
		}
	}
	return true;
}

//! Which rows of the chunks pass a predicate on one column. NULL passes only null tests.
static vector<bool> Evaluate(ClientContext &context, const Expression &pred, const Chunks &chunks, idx_t size) {
	// The predicate reads the only column of the chunks, of the same type.
	auto copy = pred.Copy();
	bool same_type = true;
	ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
	    copy, [&](BoundReferenceExpression &ref, unique_ptr<Expression> &) {
		    ref.IndexMutable() = 0;
		    same_type = same_type && (chunks.empty() || ref.GetReturnType() == chunks[0]->data[0].GetType());
	    });
	if (!same_type) {
		throw InvalidInputException("cardinal: a filter reads its column as another type");
	}
	ExpressionExecutor executor(context, *copy);

	vector<bool> result(size, false);
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	idx_t offset = 0;
	for (auto &chunk : chunks) {
		auto selected = executor.SelectExpression(*chunk, sel);
		for (idx_t i = 0; i < selected; i++) {
			result[offset + sel.get_index(i)] = true;
		}
		offset += chunk->size();
	}
	return result;
}

//! Which states pass a predicate. Non-value states hold NULL.
static vector<bool> Matches(ClientContext &context, const Expression &pred, const ModelColumn &column) {
	return Evaluate(context, pred, column.state_chunks, column.values.size());
}

static bool IsColumnConstant(const Expression &column, const Expression &constant) {
	return column.GetExpressionClass() == ExpressionClass::BOUND_REF &&
	       constant.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

//! For =, IN, != and NOT of = or IN: their distinct non-null constants, and whether negated.
static bool PointValues(const Expression &pred, idx_t &values, bool &negated) {
	set<string> distinct;
	auto add = [&](const Expression &constant) {
		auto &value = constant.Cast<BoundConstantExpression>().GetValue();
		if (!value.IsNull()) {
			distinct.insert(value.ToString());
		}
	};
	auto type = pred.GetExpressionType();
	if (BoundComparisonExpression::IsComparison(pred)) {
		if (type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOTEQUAL) {
			return false;
		}
		auto &comparison = pred.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		if (IsColumnConstant(left, right)) {
			add(right);
		} else if (IsColumnConstant(right, left)) {
			add(left);
		} else {
			return false;
		}
		negated = type == ExpressionType::COMPARE_NOTEQUAL;
		values = distinct.size();
		return true;
	}
	if (pred.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) {
		return false;
	}
	auto &children = pred.Cast<BoundOperatorExpression>().GetChildren();
	if (type == ExpressionType::OPERATOR_NOT) {
		auto child = children[0]->GetExpressionType();
		if ((child != ExpressionType::COMPARE_EQUAL && child != ExpressionType::COMPARE_IN) ||
		    !PointValues(*children[0], values, negated)) {
			return false;
		}
		negated = !negated;
		return true;
	}
	if (type != ExpressionType::COMPARE_IN && type != ExpressionType::COMPARE_NOT_IN) {
		return false;
	}
	for (idx_t i = 1; i < children.size(); i++) {
		if (!IsColumnConstant(*children[0], *children[i])) {
			return false;
		}
		add(*children[i]);
	}
	negated = type == ExpressionType::COMPARE_NOT_IN;
	values = distinct.size();
	return true;
}

//! Share of OTHER for =, IN, != and NOT of = or IN: each infrequent value equally frequent.
//! Its values that are infrequent: its values, minus the frequent states it keeps.
static bool PointShare(const Expression &pred, const ModelColumn &column, idx_t frequent_matches, double &share) {
	idx_t values;
	bool negated;
	if (!PointValues(pred, values, negated)) {
		return false;
	}
	auto positive = negated ? column.FrequentCount() - frequent_matches : frequent_matches;
	auto outside = values > positive ? values - positive : 0;
	auto members = MaxValue<idx_t>(column.members.GetIndex(), 1);
	auto in = double(MinValue(outside, members)) / double(members);
	share = negated ? 1 - in : in;
	return true;
}

//! Weight of each state of a column under ANDed predicates.
static vector<double> ColumnWeights(ClientContext &context, const vector<const Expression *> &preds,
                                    const ModelColumn &column, const LogicalType &type) {
	auto states = column.values.size();
	vector<bool> passes(states, true);
	for (auto pred : preds) {
		auto matches = Matches(context, *pred, column);
		for (idx_t s = 0; s < states; s++) {
			passes[s] = passes[s] && matches[s];
		}
	}

	vector<double> weights(states);
	idx_t frequent_matches = 0;
	for (idx_t s = 0; s < states; s++) {
		weights[s] = passes[s] ? 1.0 : 0.0;
		frequent_matches += passes[s] && column.kinds[s] == StateKind::VALUE;
	}

	// Only compacted columns have OTHER.
	if (!column.members.IsValid()) {
		return weights;
	}

	// One predicate of the =, IN, != kind: point share. Otherwise, bucket share.
	double share;
	if (preds.size() != 1 || !PointShare(*preds[0], column, frequent_matches, share)) {
		share = double(frequent_matches + 1) / double(column.FrequentCount() + 2);
	}
	for (idx_t s = 0; s < states; s++) {
		if (column.kinds[s] == StateKind::OTHER) {
			weights[s] = share;
		}
	}
	return weights;
}

//! Column weights already computed in this estimate, by column and predicates.
using WeightCache = map<pair<idx_t, vector<const Expression *>>, vector<double>>;

static double TermProbability(ClientContext &context, const Model &model, const Term &term,
                              const vector<LogicalType> &types, const Weights &extra, WeightCache &cache) {
	Weights weights = extra;
	for (auto &entry : term) {
		auto key = make_pair(entry.first, entry.second);
		auto hit = cache.find(key);
		if (hit == cache.end()) {
			auto computed = ColumnWeights(context, entry.second, model.Columns()[entry.first], types[entry.first]);
			hit = cache.emplace(key, std::move(computed)).first;
		}
		auto &column = weights[entry.first];
		if (column.empty()) {
			column = hit->second;
			continue;
		}
		for (idx_t s = 0; s < column.size(); s++) {
			column[s] *= hit->second[s];
		}
	}
	return model.Expectation(weights);
}

//! E[1(t1 or t2 or ...) w] = Σ over non-empty subsets S of (-1)^(|S|+1) E[1(AND of S) w].
static double UnionExpectation(ClientContext &context, const Model &model, const vector<Term> &terms,
                               const vector<LogicalType> &types, const Weights &extra) {
	double total = 0;
	WeightCache cache;
	for (idx_t mask = 1; mask < (idx_t(1) << terms.size()); mask++) {
		vector<Term> subset = {Term()};
		idx_t size = 0;
		for (idx_t i = 0; i < terms.size(); i++) {
			if (mask & (idx_t(1) << i)) {
				subset = Product(subset, {terms[i]});
				size++;
			}
		}
		auto sign = size % 2 == 1 ? 1.0 : -1.0;
		total += sign * TermProbability(context, model, subset[0], types, extra, cache);
	}
	return total;
}

//! Filters that change results: hints and runtime filters do not.
static bool Counts(const TableFilter &filter) {
	return !ExpressionFilter::IsOptionalFilter(filter);
}

unique_ptr<Expression> RequiredPart(const Expression &expr) {
	if (ExpressionFilter::IsOptionalExpression(expr)) {
		return nullptr;
	}
	auto copy = expr.Copy();
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &children = copy->Cast<BoundConjunctionExpression>().GetChildrenMutable();
		vector<unique_ptr<Expression>> kept;
		for (auto &child : children) {
			if (!ExpressionFilter::IsOptionalExpression(*child)) {
				kept.push_back(std::move(child));
			}
		}
		children = std::move(kept);
	}

	bool internal = false;
	ExpressionIterator::VisitExpression<BoundFunctionExpression>(*copy, [&](const BoundFunctionExpression &func) {
		internal = internal || TableFilterFunctions::IsTableFilterFunction(func.Function()) ||
		           ExpressionFilter::GetOptionalFilterChild(func);
	});
	return internal ? nullptr : std::move(copy);
}

bool PrepareFilters(LogicalGet &get, const vector<unique_ptr<Expression>> &filters, const Model &model,
                    PreparedFilters &out) {
	// The table's types, by model column. Fanout columns are never filtered.
	out.types.assign(model.Columns().size(), LogicalType::DOUBLE);
	for (idx_t i = 0; i < get.names.size(); i++) {
		auto column = model.Find(get.names[i].GetIdentifierName());
		if (column.IsValid()) {
			out.types[column.GetIndex()] = get.returned_types[i];
		}
	}

	out.filters.clear();
	if (get.table_filters.HasMultiColumnFilters()) {
		return false;
	}
	for (auto &entry : get.table_filters) {
		if (!Counts(entry.Filter())) {
			continue;
		}
		// e.g. rowid < 10: no model column.
		auto &index = get.GetColumnIndex(entry.GetIndex());
		if (index.IsVirtualColumn() || index.GetPrimaryIndex() >= get.names.size()) {
			return false;
		}
		auto primary = index.GetPrimaryIndex();
		auto column = model.Find(get.names[primary].GetIdentifierName());
		if (!column.IsValid()) {
			return false;
		}
		BoundReferenceExpression ref(get.returned_types[primary], column.GetIndex());
		auto expr = RequiredPart(*entry.Filter().ToExpression(ref));
		if (!expr) {
			return false;
		}
		out.filters.push_back(std::move(expr));
	}
	for (auto &expr : filters) {
		auto copy = Normalize(*expr, get, model);
		if (!copy) {
			return false;
		}
		out.filters.push_back(std::move(copy));
	}

	// e.g. "(#2 >= 5) AND (#0 ~~ 'A%')"
	vector<string> texts;
	for (auto &expr : out.filters) {
		texts.push_back(expr->ToString());
	}
	std::sort(texts.begin(), texts.end());
	out.key = StringUtil::Join(texts, " AND ");
	return true;
}

bool Expect(ClientContext &context, const Model &model, const PreparedFilters &filters, const Powers &powers,
            double &result) {
	// e.g. "(#2 >= 5) | #6^2"
	auto key = filters.key + " |";
	for (auto &entry : powers) {
		key += " #" + to_string(entry.first) + "^" + to_string(entry.second);
	}
	if (model.FindSelectivity(key, result)) {
		return true;
	}

	// Fanout weights: a bin weighs its mean count to the power.
	Weights extra(model.Columns().size());
	for (auto &entry : powers) {
		auto &column = model.Columns()[entry.first];
		for (auto &value : column.values) {
			extra[entry.first].push_back(std::pow(value.GetValue<double>(), double(entry.second)));
		}
	}

	vector<Term> terms = {Term()};
	for (auto &expr : filters.filters) {
		vector<Term> dnf;
		if (!DNF(*expr, dnf)) {
			return false;
		}
		terms = Product(terms, dnf);
		if (terms.size() > MAX_TERMS) {
			return false;
		}
	}

	result = MaxValue(UnionExpectation(context, model, terms, filters.types, extra), 0.0);
	model.AddSelectivity(key, result);
	return true;
}

vector<string> FilteredColumns(LogicalGet &get, const vector<unique_ptr<Expression>> &filters) {
	set<string> names;
	auto supported = [&](idx_t column) {
		return column < get.names.size() && SupportedType(get.returned_types[column]);
	};
	for (auto &entry : get.table_filters) {
		auto primary = get.GetColumnIndex(entry.GetIndex()).GetPrimaryIndex();
		if (Counts(entry.Filter()) && supported(primary)) {
			names.insert(get.names[primary].GetIdentifierName());
		}
	}
	auto &ids = get.GetColumnIds();
	for (auto &expr : filters) {
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(*expr, [&](const BoundColumnRefExpression &ref) {
			if (ref.Binding().table_index != get.table_index || ref.Binding().column_index >= ids.size()) {
				return;
			}
			auto primary = ids[ref.Binding().column_index].GetPrimaryIndex();
			if (supported(primary)) {
				names.insert(get.names[primary].GetIdentifierName());
			}
		});
	}
	return vector<string>(names.begin(), names.end());
}

optional_idx BNEstimate(ClientContext &context, LogicalGet &get, const vector<unique_ptr<Expression>> &filters,
                        idx_t rows) {
	auto table = get.GetTable();
	if (!table) {
		return optional_idx();
	}
	auto columns = FilteredColumns(get, filters);
	if (columns.empty()) {
		return rows;
	}
	AddFilterColumns(context, *table, columns);
	auto model = GetModel(context, *table);
	if (!model) {
		return optional_idx();
	}
	PreparedFilters prepared;
	double p;
	if (!PrepareFilters(get, filters, *model, prepared) || !Expect(context, *model, prepared, {}, p)) {
		return optional_idx();
	}
	return ToCount(double(rows) * p);
}

} // namespace cardinal
} // namespace duckdb
