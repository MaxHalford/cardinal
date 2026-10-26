//===----------------------------------------------------------------------===//
// Fit a table model.
//
//   1. Sample: rows whose rowid hashes below a threshold, read into memory.
//      Deterministic and parallel, unlike reservoir sampling.
//   2. Compact: a column with more than MAX_STATES distinct values keeps its
//      most frequent ones, up to max_categories - 1 or until they cover
//      `coverage` of its non-null rows. The rest share the OTHER state.
//      Values are told apart by their 64-bit hash.
//   3. Encode: each sampled row's state per column.
//   4. Structure: mutual information of each pair of columns, then the
//      maximum spanning tree (Kruskal), rooted at the first column.
//   5. Probabilities: P(state | parent state) from counts.
//
// Fanout columns count, per row, the rows of another table that reference
// it. They are binned rather than compacted: FANOUT_BINS bins of equal share
// of join output, and an exact bin for 0. e.g. one movie with 1,000 cast rows
// outweighs 500 movies with 1 each.
//
// One query reads the sample. The rest is done in memory: hashing, ranking,
// encoding, and one pass over the codes per pair of columns.
//===----------------------------------------------------------------------===//

#include "fit.hpp"
#include "inner_query.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {
namespace cardinal {

//! Columns with more distinct values than this are compacted.
static constexpr idx_t MAX_STATES = 1000;

//! Largest 64-bit hash: a row is sampled when hash(rowid) < share x MAX_HASH.
static constexpr double MAX_HASH = 18446744073709551615.0;

//! Bins per fanout column, besides the bin of 0.
static constexpr idx_t FANOUT_BINS = 16;

//! State codes, by column then sampled row.
using Codes = vector<vector<uint32_t>>;

static string Quote(const string &name) {
	return SQLIdentifier::ToString(name);
}

//! The table's name in SQL: "catalog"."schema"."table".
static string TableName(TableCatalogEntry &table) {
	return Quote(table.ParentCatalog().GetName().GetIdentifierName()) + "." +
	       Quote(table.ParentSchema().name.GetIdentifierName()) + "." + Quote(table.name.GetIdentifierName());
}

//! A sampled column: each row's value hash, and whether it is NULL.
struct Hashes {
	vector<uint64_t> hash;
	vector<bool> null;
};

//! A distinct value of a sampled column: its rows, and their weight.
struct Distinct {
	uint64_t hash;
	idx_t count;
	double weight;
};

//! Pass 1: hash every sampled value.
static vector<Hashes> HashColumns(ColumnDataCollection &sample, idx_t count) {
	vector<Hashes> columns(count);
	Vector hashes(LogicalType::HASH);
	for (auto &chunk : sample.Chunks()) {
		for (idx_t c = 0; c < columns.size(); c++) {
			VectorOperations::Hash(chunk.data[c], hashes, chunk.size());
			hashes.Flatten();
			auto data = FlatVector::GetData<hash_t>(hashes);
			UnifiedVectorFormat format;
			chunk.data[c].ToUnifiedFormat(format);
			for (idx_t r = 0; r < chunk.size(); r++) {
				columns[c].hash.push_back(data[r]);
				columns[c].null.push_back(!format.validity.RowIsValid(format.sel->get_index(r)));
			}
		}
	}
	return columns;
}

//! Distinct non-null values, heaviest first: a row weighs 1 plus the rows that join to it.
//! e.g. each keyword is one row of keyword, but 'sequel' joins thousands of movie_keyword
//! rows: it is kept, as join estimates need. Ties go by hash: a deterministic sample. e.g.
//! unique timestamps: the kept ones spread over the whole range, so the OTHER share of a
//! range filter follows the range.
static vector<Distinct> Rank(const Hashes &column, const vector<double> &weights) {
	vector<pair<uint64_t, double>> sorted;
	for (idx_t r = 0; r < column.hash.size(); r++) {
		if (!column.null[r]) {
			sorted.emplace_back(column.hash[r], weights[r]);
		}
	}
	std::sort(sorted.begin(), sorted.end());

	vector<Distinct> distinct;
	for (auto &entry : sorted) {
		if (distinct.empty() || distinct.back().hash != entry.first) {
			distinct.push_back({entry.first, 0, 0});
		}
		distinct.back().count++;
		distinct.back().weight += entry.second;
	}
	std::stable_sort(distinct.begin(), distinct.end(),
	                 [](const Distinct &a, const Distinct &b) { return a.weight > b.weight; });
	return distinct;
}

//! How many of the ranked values a compacted column keeps.
static idx_t KeptCount(const vector<Distinct> &ranked, const FitOptions &options) {
	auto limit = MinValue<idx_t>(ranked.size(), options.max_categories > 0 ? options.max_categories - 1 : 0);
	double weight = 0;
	for (auto &distinct : ranked) {
		weight += distinct.weight;
	}

	// Include the value that brings the cumulative share of the weight to the coverage.
	double total = 0;
	for (idx_t i = 0; i < limit; i++) {
		if (total / weight >= options.coverage) {
			return i;
		}
		total += ranked[i].weight;
	}
	return limit;
}

//! Step 2 for one column: its states, values still unset. Codes: kept values, then OTHER, then NULL.
static ModelColumn States(const FitColumn &fit, const LogicalType &type, const vector<Distinct> &ranked, idx_t non_null,
                          idx_t rows, const FitOptions &options) {
	// The whole table may have more distinct values than the sample.
	auto distinct = MaxValue<idx_t>(fit.distinct.IsValid() ? fit.distinct.GetIndex() : 0, ranked.size());
	bool compact = distinct > MAX_STATES;
	auto kept = compact ? KeptCount(ranked, options) : ranked.size();

	ModelColumn col;
	col.name = fit.name;
	col.type = type;
	col.values.resize(kept);
	col.kinds.resize(kept, StateKind::VALUE);
	if (compact) {
		col.members = MaxValue<idx_t>(distinct - kept, 1);
		col.values.push_back(Value());
		col.kinds.push_back(StateKind::OTHER);
	}
	if (non_null < rows) {
		col.values.push_back(Value());
		col.kinds.push_back(StateKind::NULL_VALUE);
	}
	return col;
}

//! Pass 2: each row's code, and the value of each kept state from its first row.
static Codes Encode(ColumnDataCollection &sample, const vector<Hashes> &hashes, const vector<vector<Distinct>> &ranked,
                    vector<ModelColumn> &columns) {
	Codes codes(columns.size());
	vector<unordered_map<uint64_t, uint32_t>> code_of(columns.size());
	vector<uint32_t> other(columns.size()), null_code(columns.size());
	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		idx_t kept = 0;
		while (kept < col.kinds.size() && col.kinds[kept] == StateKind::VALUE) {
			code_of[c][ranked[c][kept].hash] = uint32_t(kept);
			kept++;
		}
		other[c] = uint32_t(kept);
		null_code[c] = uint32_t(col.values.size() - 1);
		codes[c].reserve(hashes[c].hash.size());
	}

	idx_t offset = 0;
	vector<vector<bool>> filled(columns.size());
	for (idx_t c = 0; c < columns.size(); c++) {
		filled[c].resize(columns[c].values.size(), false);
	}
	for (auto &chunk : sample.Chunks()) {
		for (idx_t c = 0; c < columns.size(); c++) {
			for (idx_t r = 0; r < chunk.size(); r++) {
				auto row = offset + r;
				if (hashes[c].null[row]) {
					codes[c].push_back(null_code[c]);
					continue;
				}
				auto hit = code_of[c].find(hashes[c].hash[row]);
				if (hit == code_of[c].end()) {
					codes[c].push_back(other[c]);
					continue;
				}
				codes[c].push_back(hit->second);
				if (!filled[c][hit->second]) {
					filled[c][hit->second] = true;
					columns[c].values[hit->second] = chunk.GetValue(c, r);
				}
			}
		}
		offset += chunk.size();
	}
	return codes;
}

//! A fanout column from its counts: bins as states, valued by their mean count.
//! A bin holds the counts up to its edge: (previous edge, edge].
static ModelColumn BinFanout(const string &name, const vector<int64_t> &counts, vector<uint32_t> &codes) {
	vector<int64_t> positive;
	for (auto n : counts) {
		if (n > 0) {
			positive.push_back(n);
		}
	}
	std::sort(positive.begin(), positive.end());

	// Few distinct counts: one exact bin each. Otherwise, edges at equal shares of the
	// cumulative count, each closed before the count that crosses it: a dominant count
	// starts a bin of its own.
	vector<int64_t> edges(positive.begin(), positive.end());
	edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
	if (edges.size() > FANOUT_BINS) {
		vector<double> share(positive.size());
		double total = 0, sum = 0;
		for (auto n : positive) {
			total += double(n);
		}
		for (idx_t i = 0; i < positive.size(); i++) {
			sum += double(positive[i]);
			share[i] = sum / total;
		}
		edges.clear();
		for (idx_t b = 1; b < FANOUT_BINS; b++) {
			auto cut = idx_t(std::lower_bound(share.begin(), share.end(), double(b) / FANOUT_BINS) - share.begin());
			edges.push_back(positive[cut > 0 ? cut - 1 : 0]);
		}
		edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
	}

	codes.clear();
	idx_t bins = 1;
	for (auto n : counts) {
		uint32_t code = 0;
		if (n > 0) {
			code = uint32_t(std::lower_bound(edges.begin(), edges.end(), n) - edges.begin()) + 1;
		}
		codes.push_back(code);
		bins = MaxValue<idx_t>(bins, code + 1);
	}

	vector<double> sums(bins, 0), sizes(bins, 0);
	for (idx_t r = 0; r < counts.size(); r++) {
		sums[codes[r]] += double(counts[r]);
		sizes[codes[r]]++;
	}
	ModelColumn col;
	col.name = name;
	col.type = LogicalType::DOUBLE;
	for (idx_t b = 0; b < bins; b++) {
		col.values.push_back(Value::DOUBLE(sizes[b] > 0 ? sums[b] / sizes[b] : 0));
		col.kinds.push_back(StateKind::VALUE);
	}
	return col;
}

//! Joint counts of two columns: counts[a * states of v + b].
static vector<uint32_t> PairCounts(const vector<uint32_t> &u, const vector<uint32_t> &v, idx_t v_states,
                                   idx_t u_states) {
	vector<uint32_t> counts(u_states * v_states, 0);
	for (idx_t r = 0; r < u.size(); r++) {
		counts[u[r] * v_states + v[r]]++;
	}
	return counts;
}

static vector<uint32_t> Marginal(const vector<uint32_t> &u, idx_t states) {
	vector<uint32_t> counts(states, 0);
	for (auto code : u) {
		counts[code]++;
	}
	return counts;
}

//! Σ p(a, b) log(p(a, b) / (p(a) p(b))), from counts.
static double MutualInformation(const vector<uint32_t> &joint, const vector<uint32_t> &mu, const vector<uint32_t> &mv,
                                idx_t rows) {
	double n = double(rows), mi = 0;
	for (idx_t a = 0; a < mu.size(); a++) {
		for (idx_t b = 0; b < mv.size(); b++) {
			double nab = joint[a * mv.size() + b];
			if (nab > 0) {
				mi += nab / n * std::log(nab * n / (double(mu[a]) * double(mv[b])));
			}
		}
	}
	return mi;
}

//! Parent of each column in the maximum spanning tree, rooted at column 0.
static vector<optional_idx> Structure(const Codes &codes, const vector<ModelColumn> &columns) {
	auto count = columns.size();
	auto rows = codes[0].size();
	vector<vector<uint32_t>> marginals;
	for (idx_t i = 0; i < count; i++) {
		marginals.push_back(Marginal(codes[i], columns[i].values.size()));
	}

	struct Edge {
		idx_t u, v;
		double mi;
	};
	vector<Edge> edges;
	for (idx_t u = 0; u < count; u++) {
		for (idx_t v = u + 1; v < count; v++) {
			auto joint = PairCounts(codes[u], codes[v], columns[v].values.size(), columns[u].values.size());
			edges.push_back({u, v, MutualInformation(joint, marginals[u], marginals[v], rows)});
		}
	}
	std::stable_sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.mi > b.mi; });

	// Kruskal: the heaviest edges that join two components.
	vector<idx_t> component(count);
	for (idx_t i = 0; i < count; i++) {
		component[i] = i;
	}
	vector<vector<idx_t>> neighbors(count);
	for (auto &edge : edges) {
		auto a = component[edge.u], b = component[edge.v];
		if (a == b) {
			continue;
		}
		for (auto &c : component) {
			c = c == b ? a : c;
		}
		neighbors[edge.u].push_back(edge.v);
		neighbors[edge.v].push_back(edge.u);
	}

	// Orient from the root, breadth first.
	vector<optional_idx> parent(count);
	vector<bool> seen(count, false);
	vector<idx_t> queue = {0};
	seen[0] = true;
	for (idx_t i = 0; i < queue.size(); i++) {
		for (auto next : neighbors[queue[i]]) {
			if (seen[next]) {
				continue;
			}
			seen[next] = true;
			parent[next] = queue[i];
			queue.push_back(next);
		}
	}
	return parent;
}

//! P(state | parent state) = count(parent state, state) / count(parent state). Roots: count(state) / rows.
static void Probabilities(const Codes &codes, ModelColumn &col, idx_t i, const vector<ModelColumn> &columns) {
	auto states = col.values.size();
	if (!col.parent.IsValid()) {
		auto counts = Marginal(codes[i], states);
		for (idx_t s = 0; s < states; s++) {
			if (counts[s] > 0) {
				col.probabilities.push_back({s, 0, double(counts[s]) / double(codes[i].size())});
			}
		}
		return;
	}

	auto p = col.parent.GetIndex();
	auto parent_states = columns[p].values.size();
	auto joint = PairCounts(codes[p], codes[i], states, parent_states);
	auto parents = Marginal(codes[p], parent_states);
	for (idx_t a = 0; a < parent_states; a++) {
		for (idx_t s = 0; s < states; s++) {
			auto n = joint[a * states + s];
			if (n > 0) {
				col.probabilities.push_back({s, a, double(n) / double(parents[a])});
			}
		}
	}
}

shared_ptr<Model> FitModel(Connection &con, TableCatalogEntry &table, const vector<FitColumn> &fits,
                           const vector<FitFanout> &fanouts, const FitOptions &options) {
	auto from = TableName(table);
	auto rows = idx_t(QueryRows(*con.Query("SELECT count(*) FROM " + from)).GetValue(0, 0).GetValue<int64_t>());

	// 1. Sample. Small tables are taken whole. Fanouts: one LEFT JOIN of counts per foreign key.
	vector<string> names, joins;
	for (auto &fit : fits) {
		names.push_back("t." + Quote(fit.name));
	}
	for (idx_t i = 0; i < fanouts.size(); i++) {
		auto &fanout = fanouts[i];
		auto alias = "f" + to_string(i);
		auto fk = Quote(fanout.fk_column);
		names.push_back("coalesce(" + alias + ".n, 0)::BIGINT");
		joins.push_back(" LEFT JOIN (SELECT " + fk + " AS k, count(*) AS n FROM " + TableName(fanout.fk_table.get()) +
		                " WHERE " + fk + " IS NOT NULL GROUP BY 1) " + alias + " ON t." + Quote(fanout.pk) + " = " +
		                alias + ".k");
	}
	string where;
	if (options.sample_size > 0 && rows > options.sample_size) {
		auto threshold = uint64_t(double(options.sample_size) / double(rows) * MAX_HASH);
		where = " WHERE hash(t.rowid) < " + to_string(threshold) + "::UBIGINT";
	}
	auto result = con.Query("SELECT " + StringUtil::Join(names, ", ") + " FROM " + from + " t" +
	                        StringUtil::Join(joins, "") + where);
	if (result->HasError()) {
		throw InvalidInputException("cardinal: %s", result->GetError());
	}
	auto &sample = result->Collection();
	auto sample_rows = sample.Count();
	if (sample_rows == 0) {
		throw InvalidInputException("cardinal: %s is empty", table.name);
	}

	// 2. States. A row weighs 1 plus the rows that join to it.
	vector<vector<int64_t>> fanout_counts(fanouts.size());
	for (idx_t i = 0; i < fanouts.size(); i++) {
		for (auto &chunk : sample.Chunks()) {
			UnifiedVectorFormat format;
			chunk.data[fits.size() + i].ToUnifiedFormat(format);
			auto data = UnifiedVectorFormat::GetData<int64_t>(format);
			for (idx_t r = 0; r < chunk.size(); r++) {
				fanout_counts[i].push_back(data[format.sel->get_index(r)]);
			}
		}
	}
	vector<double> weights(sample_rows, 1.0);
	for (auto &counts : fanout_counts) {
		for (idx_t r = 0; r < sample_rows; r++) {
			weights[r] += double(counts[r]);
		}
	}

	auto hashes = HashColumns(sample, fits.size());
	vector<vector<Distinct>> ranked;
	vector<ModelColumn> columns;
	for (idx_t i = 0; i < fits.size(); i++) {
		ranked.push_back(Rank(hashes[i], weights));
		idx_t non_null = 0;
		for (auto &distinct : ranked.back()) {
			non_null += distinct.count;
		}
		columns.push_back(States(fits[i], sample.Types()[i], ranked.back(), non_null, sample_rows, options));
	}

	// 3. Codes.
	auto codes = Encode(sample, hashes, ranked, columns);
	hashes.clear();
	for (idx_t i = 0; i < fanouts.size(); i++) {
		codes.emplace_back();
		columns.push_back(
		    BinFanout(FanoutName(fanouts[i].fk_table.get().name.GetIdentifierName(), fanouts[i].fk_column),
		              fanout_counts[i], codes.back()));
	}
	result.reset();

	// 4. Tree.
	auto parents = Structure(codes, columns);
	for (idx_t i = 0; i < columns.size(); i++) {
		columns[i].parent = parents[i];
	}

	// 5. Probabilities.
	for (idx_t i = 0; i < columns.size(); i++) {
		Probabilities(codes, columns[i], i, columns);
	}
	return make_shared_ptr<Model>(rows, std::move(columns));
}

bool SupportedType(const LogicalType &type) {
	return !type.IsNested() && type.id() != LogicalTypeId::BLOB;
}

vector<FitColumn> FitColumns(ClientContext &context, TableCatalogEntry &table, const vector<string> &names) {
	vector<string> chosen;
	for (auto &column : table.GetColumns().Logical()) {
		auto named = names.empty() || std::find(names.begin(), names.end(), column.Name()) != names.end();
		if (named && SupportedType(column.Type())) {
			chosen.push_back(column.Name().GetIdentifierName());
		}
	}
	if (chosen.empty()) {
		throw InvalidInputException("cardinal: %s has no column to model", table.name);
	}

	// Distinct counts: DuckDB keeps a HyperLogLog sketch per column.
	vector<FitColumn> columns;
	for (auto &name : chosen) {
		FitColumn column {name, optional_idx()};
		auto &definition = table.GetColumns().GetColumn(Identifier(name));
		auto stats = table.GetStatistics(context, definition.Logical().index);
		if (stats && stats->GetDistinctCount() > 0) {
			column.distinct = stats->GetDistinctCount();
		}
		columns.push_back(column);
	}
	return columns;
}

} // namespace cardinal
} // namespace duckdb
