#include "join_planner.hpp"
#include "exact.hpp"
#include "inner_query.hpp"
#include "join_estimator.hpp"
#include "keys.hpp"
#include "models.hpp"
#include "query_graph.hpp"

#include "scan_hook.hpp"

#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

//! Connected sets fitted at most, smallest first.
static constexpr idx_t MAX_SETS = 2000;

//! A filtered domain with at most this many rows is read: its passing keys fold into the join.
static constexpr idx_t MAX_DIMENSION_ROWS = 10000;

//! Pull of the fit towards the starting values: only breaks ties between solutions.
static constexpr double RIDGE = 1e-6;

//! Share cardinal's plan must be believed cheaper by, to replace DuckDB's. Below it, the
//! difference is within the errors of the estimates: DuckDB keeps its plan.
static constexpr double MARGIN = 0.05;

//! Fits with the edge order of the previous solution.
static constexpr idx_t FIT_ROUNDS = 2;

static idx_t TableRows(TableCatalogEntry &table) {
	return table.IsDuckTable() ? table.GetStorage().GetTotalRows() : 0;
}

static string ColumnName(const QueryGraph &graph, const GraphColumn &column) {
	return graph.relations[column.first].get.get().GetTable()->GetColumns().GetColumn(LogicalIndex(column.second)).Name().GetIdentifierName();
}

//! Each class's primary key: the unique column with the most rows, else the one its
//! columns are known to reference. Other columns become known foreign keys.
static vector<JoinDomain> Domains(ClientContext &context, const QueryGraph &graph) {
	vector<JoinDomain> domains(graph.classes.size());
	for (idx_t c = 0; c < graph.classes.size(); c++) {
		optional_idx pk;
		idx_t pk_rows = 0;
		for (idx_t m = 0; m < graph.classes[c].size(); m++) {
			auto &column = graph.classes[c][m];
			auto &table = *graph.relations[column.first].get.get().GetTable();
			if (!IsUnique(context, table, ColumnName(graph, column))) {
				continue;
			}
			auto rows = TableRows(table);
			if (!pk.IsValid() || rows > pk_rows) {
				pk = m;
				pk_rows = rows;
			}
		}

		auto &domain = domains[c];
		if (pk.IsValid()) {
			auto &key = graph.classes[c][pk.GetIndex()];
			domain.known = true;
			domain.relation = key.first;
			domain.key = KeyColumn(*graph.relations[key.first].get.get().GetTable(), ColumnName(graph, key));
			for (auto &column : graph.classes[c]) {
				if (column != key) {
					auto &fk_table = *graph.relations[column.first].get.get().GetTable();
					KeyColumn fk(fk_table, ColumnName(graph, column));
					AddForeignKey(context, fk, domain.key);
					// Modeled: its NULL state tells which rows can join.
					AddFilterColumns(context, fk_table, {fk.column});
				}
			}
			continue;
		}

		// No primary key in the query: the one every column is known to reference.
		for (auto &column : graph.classes[c]) {
			KeyColumn fk(*graph.relations[column.first].get.get().GetTable(), ColumnName(graph, column)), pk_key;
			if (!ReferencedKey(context, fk, pk_key) || (domain.known && !(pk_key == domain.key))) {
				domain.known = false;
				break;
			}
			domain.known = true;
			domain.key = pk_key;
		}
		if (domain.known) {
			for (auto &column : graph.classes[c]) {
				AddFilterColumns(context, *graph.relations[column.first].get.get().GetTable(), {ColumnName(graph, column)});
			}
		}
	}
	return domains;
}

//! The keys of small filtered domains that pass their filters. Their foreign keys become
//! filtered columns: models then cover them.
static void DimensionKeys(ClientContext &context, const QueryGraph &graph, vector<JoinDomain> &domains) {
	for (idx_t c = 0; c < graph.classes.size(); c++) {
		auto &domain = domains[c];
		if (!domain.known || !domain.relation.IsValid()) {
			continue;
		}
		auto &relation = graph.relations[domain.relation.GetIndex()];
		auto &get = relation.get.get();
		auto &table = *get.GetTable();
		if (TableRows(table) > MAX_DIMENSION_ROWS || FilteredColumns(get, relation.filters).empty()) {
			continue;
		}
		auto result = InnerQuery(context, ScanQuery(get, relation.filters, KeywordHelper::WriteOptionallyQuoted(domain.key.column)));
		if (!result) {
			continue;
		}
		domain.folds = true;
		for (idx_t i = 0; i < result->RowCount(); i++) {
			domain.keys.push_back(result->GetValue(0, i));
		}
		for (auto &column : graph.classes[c]) {
			if (column.first != domain.relation.GetIndex()) {
				auto &fk_table = *graph.relations[column.first].get.get().GetTable();
				AddFilterColumns(context, fk_table, {ColumnName(graph, column)});
			}
		}
	}
}

//! Connected sets of relations, smallest first, at most MAX_SETS.
static vector<uint64_t> ConnectedSets(const QueryGraph &graph) {
	auto count = graph.relations.size();
	vector<uint64_t> neighbors(count, 0);
	for (auto &edge : graph.edges) {
		neighbors[edge.left.first] |= uint64_t(1) << edge.right.first;
		neighbors[edge.right.first] |= uint64_t(1) << edge.left.first;
	}

	vector<uint64_t> sets;
	unordered_set<uint64_t> seen;
	for (idx_t r = 0; r < count; r++) {
		sets.push_back(uint64_t(1) << r);
		seen.insert(sets.back());
	}
	for (idx_t i = 0; i < sets.size() && sets.size() < MAX_SETS; i++) {
		uint64_t around = 0;
		for (idx_t r = 0; r < count; r++) {
			if (sets[i] & (uint64_t(1) << r)) {
				around |= neighbors[r];
			}
		}
		around &= ~sets[i];
		for (idx_t r = 0; r < count && sets.size() < MAX_SETS; r++) {
			auto bigger = sets[i] | (uint64_t(1) << r);
			if ((around & (uint64_t(1) << r)) && seen.insert(bigger).second) {
				sets.push_back(bigger);
			}
		}
	}
	return sets;
}

//! DuckDB's formula for a set: coefficients of θ = (n per relation, d per class), and offset.
static void FormulaRow(const QueryGraph &graph, uint64_t members, const vector<double> &d, vector<double> &x,
                       double &offset) {
	auto count = graph.relations.size();
	x.assign(count + graph.classes.size(), 0);
	idx_t components = 0;
	vector<idx_t> parent(count);
	for (idx_t r = 0; r < count; r++) {
		parent[r] = r;
		if (members & (uint64_t(1) << r)) {
			x[r] = 1;
			components++;
		}
	}
	std::function<idx_t(idx_t)> find = [&](idx_t v) {
		return parent[v] == v ? v : find(parent[v]);
	};

	vector<idx_t> order(graph.edges.size());
	for (idx_t e = 0; e < order.size(); e++) {
		order[e] = e;
	}
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return d[graph.edge_class[a]] > d[graph.edge_class[b]]; });

	set<idx_t> unused;
	for (auto e : order) {
		auto &edge = graph.edges[e];
		auto a = edge.left.first, b = edge.right.first;
		if (!(members & (uint64_t(1) << a)) || !(members & (uint64_t(1) << b))) {
			continue;
		}
		auto c = graph.edge_class[e];
		if (components == 1) {
			unused.insert(c);
			continue;
		}
		if (find(a) != find(b)) {
			parent[find(a)] = find(b);
			components--;
			x[count + c] -= 1;
		}
	}
	offset = -std::log(1.0 + double(unused.size()));
}

//! Solves (XᵀX + λI) θ = Xᵀy + λθ0 by Gaussian elimination.
static vector<double> RidgeSolve(const vector<vector<double>> &X, const vector<double> &y, const vector<double> &theta0) {
	auto n = theta0.size();
	vector<vector<double>> a(n, vector<double>(n + 1, 0));
	for (idx_t i = 0; i < n; i++) {
		a[i][i] = RIDGE;
		a[i][n] = RIDGE * theta0[i];
	}
	for (idx_t k = 0; k < X.size(); k++) {
		for (idx_t i = 0; i < n; i++) {
			if (X[k][i] == 0) {
				continue;
			}
			for (idx_t j = 0; j < n; j++) {
				a[i][j] += X[k][i] * X[k][j];
			}
			a[i][n] += X[k][i] * y[k];
		}
	}
	for (idx_t col = 0; col < n; col++) {
		idx_t pivot = col;
		for (idx_t row = col + 1; row < n; row++) {
			if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
				pivot = row;
			}
		}
		std::swap(a[col], a[pivot]);
		for (idx_t row = 0; row < n; row++) {
			if (row == col || a[row][col] == 0) {
				continue;
			}
			auto factor = a[row][col] / a[col][col];
			for (idx_t j = col; j <= n; j++) {
				a[row][j] -= factor * a[col][j];
			}
		}
	}
	vector<double> theta(n);
	for (idx_t i = 0; i < n; i++) {
		theta[i] = a[i][n] / a[i][i];
	}
	return theta;
}

//! The relations under op, and the relation set of each join in it.
static uint64_t JoinSets(LogicalOperator &op, const unordered_map<TableIndex, idx_t> &relation_of, vector<uint64_t> &joins) {
	uint64_t set = 0;
	for (auto &child : op.children) {
		set |= JoinSets(*child, relation_of, joins);
	}
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto hit = relation_of.find(op.Cast<LogicalGet>().table_index);
		if (hit != relation_of.end()) {
			set |= uint64_t(1) << hit->second;
		}
	}
	bool join = op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	            op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT || op.type == LogicalOperatorType::LOGICAL_ANY_JOIN;
	if (join && __builtin_popcountll(set) > 1) {
		joins.push_back(set);
	}
	return set;
}

//! The joins of the order DuckDB picks for the analyzed plan, with the targets or without.
static vector<uint64_t> DuckDBJoins(ClientContext &context, const QueryGraph &graph, optional_ptr<JoinTargets> targets) {
	unordered_map<TableIndex, idx_t> relation_of;
	for (idx_t r = 0; r < graph.relations.size(); r++) {
		relation_of[graph.relations[r].get.get().table_index] = r;
	}
	auto copy = graph.plan->Copy(context);
	if (targets) {
		AttachTargets(context, *copy, *targets);
	}
	try {
		JoinOrderOptimizer optimizer(context);
		copy = optimizer.Optimize(std::move(copy));
	} catch (std::exception &) {
		if (targets) {
			DetachScanHooks();
		}
		throw;
	}
	if (targets) {
		DetachScanHooks();
	}
	vector<uint64_t> joins;
	JoinSets(*copy, relation_of, joins);
	return joins;
}

//! Targets that leave the query to DuckDB.
static JoinTargets HandsOff() {
	JoinTargets targets;
	targets.hands_off = true;
	return targets;
}

JoinTargets PlanJoins(ClientContext &context, Optimizer &optimizer, LogicalOperator &plan) {
	JoinTargets targets;
	auto graph = BuildGraph(optimizer, plan);
	auto count = graph.relations.size();
	// One scan: no join order to get wrong, the scan estimate stands.
	if (count < 2) {
		return targets;
	}
	// Joins out of reach: DuckDB's estimates, not base estimates with DuckDB's join formula.
	if (count > 64 || graph.edges.empty()) {
		return HandsOff();
	}

	// What the query teaches first: models then cover its filters and fanouts.
	for (auto &relation : graph.relations) {
		auto &get = relation.get.get();
		AddFilterColumns(context, *get.GetTable(), FilteredColumns(get, relation.filters));
	}
	auto domains = Domains(context, graph);
	DimensionKeys(context, graph, domains);

	// Relations, and their estimates after filters.
	vector<JoinRelation> relations;
	vector<double> base(count);
	for (idx_t r = 0; r < count; r++) {
		auto &get = graph.relations[r].get.get();
		auto &table = *get.GetTable();
		JoinRelation relation {table, GetModel(context, table), {}, TableRows(table)};
		double p = 1.0;
		bool filtered = !FilteredColumns(get, graph.relations[r].filters).empty();
		if (relation.model) {
			// Even without filters: the model's column types, for its foreign keys.
			if (!PrepareFilters(get, graph.relations[r].filters, *relation.model, relation.filters) ||
			    !Expect(context, *relation.model, relation.filters, {}, p)) {
				return HandsOff();
			}
		} else if (filtered) {
			return HandsOff();
		}
		base[r] = MaxValue(double(relation.rows) * p, 1.0);
		relations.push_back(std::move(relation));
	}

	// Observations: base estimates, and fanout estimates of connected sets.
	JoinEstimator estimator(context, graph, std::move(relations), std::move(domains));
	vector<uint64_t> sets;
	vector<double> y;
	idx_t joins = 0;
	for (auto set : ConnectedSets(graph)) {
		double card;
		if (__builtin_popcountll(set) == 1) {
			card = base[__builtin_ctzll(set)];
		} else if (!estimator.Estimate(set, card) || card <= 0) {
			continue;
		} else {
			joins++;
		}
		sets.push_back(set);
		y.push_back(std::log(MaxValue(card, 1.0)));
	}
	if (joins == 0) {
		return HandsOff();
	}

	// Start: DuckDB's own values. Distinct counts from its statistics, the largest per class.
	vector<double> theta(count + graph.classes.size());
	for (idx_t r = 0; r < count; r++) {
		theta[r] = std::log(base[r]);
	}
	for (idx_t c = 0; c < graph.classes.size(); c++) {
		double distinct = 1;
		for (auto &column : graph.classes[c]) {
			auto &table = *graph.relations[column.first].get.get().GetTable();
			auto stats = table.GetStatistics(context, column.second);
			distinct = MaxValue(distinct, stats ? double(stats->GetDistinctCount()) : 1.0);
		}
		theta[count + c] = std::log(distinct);
	}

	auto start = theta;
	for (idx_t round = 0; round < FIT_ROUNDS; round++) {
		vector<double> d(theta.begin() + count, theta.end());
		vector<vector<double>> X(sets.size());
		vector<double> target(sets.size());
		for (idx_t i = 0; i < sets.size(); i++) {
			double offset;
			FormulaRow(graph, sets[i], d, X[i], offset);
			target[i] = y[i] - offset;
		}
		theta = RidgeSolve(X, target, start);
	}

	for (idx_t r = 0; r < count; r++) {
		targets.rows[graph.relations[r].get.get().table_index] = MaxValue(std::exp(theta[r]), 1.0);
	}
	for (idx_t c = 0; c < graph.classes.size(); c++) {
		auto distinct = idx_t(MaxValue(std::round(std::exp(theta[count + c])), 1.0));
		for (auto &column : graph.classes[c]) {
			targets.distinct[make_pair(graph.relations[column.first].get.get().table_index, column.second)] = distinct;
		}
	}

	// Do no harm: keep the targets only if DuckDB's plan with them is believed cheaper, by
	// cardinal's own estimates, than DuckDB's plan without them. Belief: the fanout estimate,
	// else the fitted formula. Cost: the sum of intermediate join sizes, as DuckDB's.
	vector<double> d(theta.begin() + count, theta.end());
	auto belief = [&](uint64_t set) {
		double card;
		if (estimator.Estimate(set, card)) {
			return MaxValue(card, 1.0);
		}
		vector<double> x;
		double offset;
		FormulaRow(graph, set, d, x, offset);
		for (idx_t i = 0; i < x.size(); i++) {
			offset += x[i] * theta[i];
		}
		return std::exp(offset);
	};
	// Intermediate joins only: each tree's root, the result, is the same for every plan.
	auto cost = [&](const vector<uint64_t> &joins) {
		double total = 0;
		for (auto set : joins) {
			bool inner = false;
			for (auto other : joins) {
				inner = inner || (other != set && (other & set) == set);
			}
			if (inner) {
				total += belief(set);
			}
		}
		return total;
	};
	try {
		auto own = cost(DuckDBJoins(context, graph, nullptr));
		auto ours = cost(DuckDBJoins(context, graph, &targets));
		if (ours < (1 - MARGIN) * own) {
			return targets;
		}
	} catch (std::exception &) {
	}
	return HandsOff();
}

} // namespace duckdb
