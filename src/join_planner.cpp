#include "join_planner.hpp"
#include "exact.hpp"
#include "inner_query.hpp"
#include "join_estimator.hpp"
#include "keys.hpp"
#include "models.hpp"
#include "query_graph.hpp"

#include "scan_hook.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/storage/data_table.hpp"

#include <bitset>

namespace duckdb {
namespace cardinal {

//! Connected sets fitted at most, smallest first.
static constexpr idx_t MAX_SETS = 2000;

//! Share of a query's joins cardinal must estimate to plan it. Below, DuckDB plans alone.
static constexpr double MIN_COVERAGE = 0.5;

//! A filtered domain with at most this many rows is read: its passing keys fold into the join.
static constexpr idx_t MAX_DIMENSION_ROWS = 10000;

//! Relations in a set, e.g. 3 for 0b1011. Portable: MSVC lacks __builtin_popcountll.
static idx_t RelationCount(uint64_t set) {
	return std::bitset<64>(set).count();
}

//! Pull of the fit towards the starting values: only breaks ties between solutions.
static constexpr double RIDGE = 1e-6;

//! Fits with the edge order of the previous solution.
static constexpr idx_t FIT_ROUNDS = 2;

static idx_t TableRows(TableCatalogEntry &table) {
	return table.IsDuckTable() ? table.GetStorage().GetTotalRows() : 0;
}

static string ColumnName(const QueryGraph &graph, const GraphColumn &column) {
	return graph.relations[column.first]
	    .get.get()
	    .GetTable()
	    ->GetColumns()
	    .GetColumn(LogicalIndex(column.second))
	    .Name()
	    .GetIdentifierName();
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
					// A self-join on the key, e.g. t1.id = t2.id: no foreign key.
					if (fk.ToString() == domain.key.ToString()) {
						continue;
					}
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
		// Known key or not, the models cover the columns: without a key, the relations join
		// through these columns' values.
		for (auto &column : graph.classes[c]) {
			AddFilterColumns(context, *graph.relations[column.first].get.get().GetTable(), {ColumnName(graph, column)});
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
		if (TableRows(table) > MAX_DIMENSION_ROWS) {
			continue;
		}
		// Small: it folds under a filter across relations too, with the keys passing it.
		domain.small = true;
		if (!FilteredColumns(get, relation.filters).empty()) {
			auto result =
			    InnerQuery(context, ScanQuery(get, relation.filters, SQLIdentifier::ToString(domain.key.column)));
			if (result) {
				domain.folds = true;
				for (idx_t i = 0; i < result->RowCount(); i++) {
					domain.keys.push_back(result->GetValue(0, i));
				}
			}
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

//! Edges in the order DuckDB's formula takes them: largest distinct count first.
static vector<idx_t> EdgeOrder(const QueryGraph &graph, const vector<double> &d) {
	vector<idx_t> order(graph.edges.size());
	for (idx_t e = 0; e < order.size(); e++) {
		order[e] = e;
	}
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return d[graph.edge_class[a]] > d[graph.edge_class[b]]; });
	return order;
}

//! DuckDB's formula for a set: coefficients of θ = (log n per relation, log d per class), and offset.
static void FormulaRow(const QueryGraph &graph, uint64_t members, const vector<double> &d, const vector<idx_t> &order,
                       vector<double> &x, double &offset) {
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

	// Edges after the set is connected: a penalty per distinct count, unless their class
	// was already used, e.g. a.x = b.x = c.x adds nothing for a.x = c.x.
	set<idx_t> used;
	set<idx_t> unused;
	for (auto e : order) {
		auto &edge = graph.edges[e];
		auto a = edge.left.first, b = edge.right.first;
		if (!(members & (uint64_t(1) << a)) || !(members & (uint64_t(1) << b))) {
			continue;
		}
		auto c = graph.edge_class[e];
		if (components == 1) {
			if (!used.count(c)) {
				unused.insert(ToCount(std::exp(d[c])));
			}
			continue;
		}
		used.insert(c);
		if (find(a) != find(b)) {
			parent[find(a)] = find(b);
			components--;
			x[count + c] -= 1;
		}
	}
	offset = -std::log(1.0 + double(unused.size()));

	// An OR across relations of the set: DuckDB applies its default selectivity.
	for (auto &filter : graph.filters) {
		if (filter.is_or && (filter.relations & ~members) == 0) {
			offset += std::log(RelationStatisticsHelper::DEFAULT_SELECTIVITY);
		}
	}
}

//! Solves (XᵀX + λI) θ = Xᵀy + λθ0 by Gaussian elimination.
static vector<double> RidgeSolve(const vector<vector<double>> &X, const vector<double> &y,
                                 const vector<double> &theta0) {
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
	// Tables DuckDB does not store, e.g. attached Postgres ones: their row counts are unknown.
	for (auto &relation : graph.relations) {
		if (!relation.get.get().GetTable()->IsDuckTable()) {
			return HandsOff();
		}
	}

	// What the query teaches first: models then cover its filters and fanouts.
	for (auto &relation : graph.relations) {
		auto &get = relation.get.get();
		AddFilterColumns(context, *get.GetTable(), FilteredColumns(get, relation.filters));
	}
	// Filters across relations: each relation's columns in them.
	for (auto &filter : graph.filters) {
		for (idx_t r = 0; r < count && filter.resolved; r++) {
			if (!(filter.relations & (uint64_t(1) << r))) {
				continue;
			}
			auto &get = graph.relations[r].get.get();
			vector<unique_ptr<Expression>> expressions;
			expressions.push_back(filter.expr->Copy());
			AddFilterColumns(context, *get.GetTable(), FilteredColumns(get, expressions));
		}
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
	idx_t joins = 0, estimated = 0;
	for (auto set : ConnectedSets(graph)) {
		double card;
		if (RelationCount(set) == 1) {
			card = base[CountZeros<uint64_t>::Trailing(set)];
		} else {
			joins++;
			if (!estimator.Estimate(set, card)) {
				continue;
			}
			estimated++;
		}
		sets.push_back(set);
		y.push_back(std::log(MaxValue(card, 1.0)));
	}
	// Mostly out of reach, e.g. joins in a cycle: DuckDB's formula would fill the rest from
	// cardinal's numbers, which they were not fitted for. DuckDB plans alone.
	if (double(estimated) < MIN_COVERAGE * double(joins)) {
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
		auto order = EdgeOrder(graph, d);
		vector<vector<double>> X(sets.size());
		vector<double> target(sets.size());
		for (idx_t i = 0; i < sets.size(); i++) {
			double offset;
			FormulaRow(graph, sets[i], d, order, X[i], offset);
			target[i] = y[i] - offset;

			// Smaller sets weigh more: join order builds on them. Weight 1 / relations, e.g.
			// a scan 1, a pair 1/2. Least squares weighs a row by the square of its scale.
			auto scale = std::sqrt(1.0 / double(RelationCount(sets[i])));
			for (auto &coefficient : X[i]) {
				coefficient *= scale;
			}
			target[i] *= scale;
		}
		theta = RidgeSolve(X, target, start);
	}

	for (idx_t r = 0; r < count; r++) {
		targets.rows[graph.relations[r].get.get().table_index] = MaxValue(std::exp(theta[r]), 1.0);
	}
	for (idx_t c = 0; c < graph.classes.size(); c++) {
		auto distinct = ToCount(std::exp(theta[count + c]));
		for (auto &column : graph.classes[c]) {
			targets.distinct[make_pair(graph.relations[column.first].get.get().table_index, column.second)] = distinct;
		}
	}

	return targets;
}

} // namespace cardinal
} // namespace duckdb
