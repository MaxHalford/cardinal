#include "model.hpp"

namespace duckdb {
namespace cardinal {

string FanoutName(const string &fk_table, const string &fk_column) {
	return "F(" + fk_table + "." + fk_column + ")";
}

idx_t ModelColumn::FrequentCount() const {
	idx_t count = 0;
	for (auto kind : kinds) {
		count += kind == StateKind::VALUE;
	}
	return count;
}

//! Values as chunks of STANDARD_VECTOR_SIZE rows. NULL values stay NULL.
static Chunks ToChunks(const vector<Value> &values, const LogicalType &type) {
	Chunks chunks;
	for (idx_t offset = 0; offset < values.size(); offset += STANDARD_VECTOR_SIZE) {
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, values.size() - offset);
		auto chunk = make_uniq<DataChunk>();
		chunk->Initialize(Allocator::DefaultAllocator(), {type});
		// Since v2, vectors hold their own size: Append sets it, the chunk derives its own.
		for (idx_t i = 0; i < count; i++) {
			auto &value = values[offset + i];
			chunk->data[0].Append(value.IsNull() ? Value(type) : value.DefaultCastAs(type));
		}
		chunks.push_back(std::move(chunk));
	}
	return chunks;
}

Model::Model(idx_t rows, vector<ModelColumn> columns_p) : rows(rows), columns(std::move(columns_p)) {
	for (auto &col : columns) {
		col.state_chunks = ToChunks(col.values, col.type);
	}
	for (idx_t i = 0; i < columns.size(); i++) {
		by_name[columns[i].name] = i;
		if (!columns[i].parent.IsValid()) {
			roots.push_back(i);
			continue;
		}
		columns[columns[i].parent.GetIndex()].children.push_back(i);
	}

	// Breadth first from the roots.
	order = roots;
	for (idx_t i = 0; i < order.size(); i++) {
		for (auto child : columns[order[i]].children) {
			order.push_back(child);
		}
	}
}

idx_t Model::Rows() const {
	return rows;
}

const vector<ModelColumn> &Model::Columns() const {
	return columns;
}

optional_idx Model::Find(const string &column) const {
	auto hit = by_name.find(column);
	if (hit == by_name.end()) {
		return optional_idx();
	}
	return hit->second;
}

//! Weighted sum-product on the tree. A column's belief over its states:
//!
//!   belief(c)[s] = weight(c)[s] · Π_children Σ_t P(t | s) · belief(child)[t]
//!
//! Subtrees without evidence sum to 1: they are skipped.
vector<double> Model::Belief(idx_t column, const Weights &weights, const vector<bool> &evidence) const {
	auto &col = columns[column];
	vector<double> belief(col.values.size(), 1.0);
	if (!weights[column].empty()) {
		belief = weights[column];
	}

	for (auto child : col.children) {
		if (!evidence[child]) {
			continue;
		}
		auto child_belief = Belief(child, weights, evidence);
		vector<double> message(col.values.size(), 0.0);
		for (auto &entry : columns[child].probabilities) {
			message[entry.parent_state] += entry.p * child_belief[entry.state];
		}
		for (idx_t s = 0; s < belief.size(); s++) {
			belief[s] *= message[s];
		}
	}
	return belief;
}

//! Cached selectivities, at most. Past it, the cache starts over.
static constexpr idx_t MAX_SELECTIVITIES = 100000;

bool Model::FindSelectivity(const string &filters, double &selectivity) const {
	lock_guard<mutex> guard(selectivity_lock);
	auto hit = selectivities.find(filters);
	if (hit == selectivities.end()) {
		return false;
	}
	selectivity = hit->second;
	return true;
}

void Model::AddSelectivity(const string &filters, double selectivity) const {
	lock_guard<mutex> guard(selectivity_lock);
	if (selectivities.size() >= MAX_SELECTIVITIES) {
		selectivities.clear();
	}
	selectivities[filters] = selectivity;
}

//! evidence[c]: some column in the subtree of c has weights. Children first.
vector<bool> Model::Evidence(const Weights &weights) const {
	vector<bool> evidence(columns.size(), false);
	for (auto it = order.rbegin(); it != order.rend(); it++) {
		auto i = *it;
		evidence[i] = evidence[i] || !weights[i].empty();
		if (evidence[i] && columns[i].parent.IsValid()) {
			evidence[columns[i].parent.GetIndex()] = true;
		}
	}
	return evidence;
}

double Model::Expectation(const Weights &weights) const {
	auto evidence = Evidence(weights);

	double result = 1.0;
	for (auto root : roots) {
		if (!evidence[root]) {
			continue;
		}
		auto belief = Belief(root, weights, evidence);
		double total = 0.0;
		for (auto &entry : columns[root].probabilities) {
			total += entry.p * belief[entry.state];
		}
		result *= total;
	}
	return result;
}

//! The rest of the tree's belief about a column's states, for its marginal:
//!
//!   outside(c)[s] = Σ_t P(s | t) · weight(p)[t] · outside(p)[t] · Π_siblings messages(t)
//!
//! where p is the parent of c. A root's outside is its prior.
vector<double> Model::Outside(idx_t column, const Weights &weights, const vector<bool> &evidence) const {
	auto &col = columns[column];
	vector<double> result(col.values.size(), 0.0);
	if (!col.parent.IsValid()) {
		for (auto &entry : col.probabilities) {
			result[entry.state] += entry.p;
		}
		return result;
	}

	// The parent's belief without this column's subtree.
	auto parent = col.parent.GetIndex();
	auto &par = columns[parent];
	auto term = Outside(parent, weights, evidence);
	if (!weights[parent].empty()) {
		for (idx_t t = 0; t < term.size(); t++) {
			term[t] *= weights[parent][t];
		}
	}
	for (auto sibling : par.children) {
		if (sibling == column || !evidence[sibling]) {
			continue;
		}
		auto belief = Belief(sibling, weights, evidence);
		vector<double> message(par.values.size(), 0.0);
		for (auto &entry : columns[sibling].probabilities) {
			message[entry.parent_state] += entry.p * belief[entry.state];
		}
		for (idx_t t = 0; t < term.size(); t++) {
			term[t] *= message[t];
		}
	}

	for (auto &entry : col.probabilities) {
		result[entry.state] += entry.p * term[entry.parent_state];
	}
	return result;
}

vector<double> Model::Marginal(idx_t column, const Weights &weights) const {
	auto evidence = Evidence(weights);
	// The column's subtree, and the rest of its tree.
	auto down = Belief(column, weights, evidence);
	auto outside = Outside(column, weights, evidence);
	for (idx_t s = 0; s < down.size(); s++) {
		down[s] *= outside[s];
	}

	// Other trees are independent: their expectations scale every state alike.
	auto root = column;
	while (columns[root].parent.IsValid()) {
		root = columns[root].parent.GetIndex();
	}
	for (auto other : roots) {
		if (other == root || !evidence[other]) {
			continue;
		}
		auto belief = Belief(other, weights, evidence);
		double total = 0.0;
		for (auto &entry : columns[other].probabilities) {
			total += entry.p * belief[entry.state];
		}
		for (auto &value : down) {
			value *= total;
		}
	}
	return down;
}

} // namespace cardinal
} // namespace duckdb
