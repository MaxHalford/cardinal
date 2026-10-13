#include "join_estimator.hpp"
#include "models.hpp"

#include "duckdb/storage/data_table.hpp"

namespace duckdb {

//! Thrown inside an estimate when the set is out of reach, e.g. a missing fanout.
struct Unsupported {};

JoinEstimator::JoinEstimator(ClientContext &context, const QueryGraph &graph, vector<JoinRelation> relations_p,
                             vector<JoinDomain> domains_p)
    : context(context), graph(graph), relations(std::move(relations_p)), domains(std::move(domains_p)) {
}

static bool Has(uint64_t set, idx_t r) {
	return set & (uint64_t(1) << r);
}

//! Relations and classes, linked by membership, form a tree.
static bool IsTree(uint64_t set, idx_t relation_count, idx_t links, idx_t class_count,
                   const vector<pair<idx_t, idx_t>> &pairs) {
	idx_t members = 0;
	for (idx_t r = 0; r < relation_count; r++) {
		members += Has(set, r);
	}
	if (links != members + class_count - 1) {
		return false;
	}
	// Connected: a tree has n - 1 links only when connected.
	vector<idx_t> parent(relation_count);
	for (idx_t r = 0; r < relation_count; r++) {
		parent[r] = r;
	}
	std::function<idx_t(idx_t)> find = [&](idx_t x) {
		return parent[x] == x ? x : find(parent[x]);
	};
	for (auto &pair : pairs) {
		parent[find(pair.first)] = find(pair.second);
	}
	optional_idx root;
	for (idx_t r = 0; r < relation_count; r++) {
		if (!Has(set, r)) {
			continue;
		}
		if (!root.IsValid()) {
			root = find(r);
		} else if (find(r) != root.GetIndex()) {
			return false;
		}
	}
	return true;
}

bool JoinEstimator::Estimate(uint64_t set, double &card) {
	// The classes of the set: graph classes with at least two members in it.
	classes.clear();
	domain_of.clear();
	fk_of.clear();
	idx_t links = 0;
	vector<pair<idx_t, idx_t>> pairs;
	for (idx_t id = 0; id < graph.classes.size(); id++) {
		map<idx_t, string> members;
		for (auto &column : graph.classes[id]) {
			if (!Has(set, column.first)) {
				continue;
			}
			if (members.count(column.first)) {
				// Two columns of one relation in a class: a selection, not a join.
				return false;
			}
			members[column.first] = relations[column.first].table.get().GetColumns().GetColumn(LogicalIndex(column.second)).Name();
		}
		if (members.size() < 2) {
			continue;
		}
		if (!domains[id].known) {
			return false;
		}
		Class c;
		c.id = id;
		auto d = domains[id].relation;
		if (d.IsValid() && members.count(d.GetIndex())) {
			c.d = d;
			members.erase(d.GetIndex());
		}
		c.fks = members;
		links += c.fks.size() + c.d.IsValid();
		idx_t first = c.d.IsValid() ? c.d.GetIndex() : c.fks.begin()->first;
		for (auto &fk : c.fks) {
			pairs.emplace_back(first, fk.first);
		}
		classes.push_back(std::move(c));
	}
	if (!IsTree(set, relations.size(), links, classes.size(), pairs)) {
		return false;
	}
	for (auto &c : classes) {
		if (c.d.IsValid()) {
			domain_of[c.d.GetIndex()] = &c;
		}
		for (auto &fk : c.fks) {
			fk_of[fk.first].push_back(&c);
		}
	}

	idx_t root = 0;
	while (!Has(set, root)) {
		root++;
	}
	try {
		card = double(relations[root].rows) * Weight(root, nullptr);
	} catch (Unsupported &) {
		return false;
	}
	return true;
}

double JoinEstimator::ExpectOn(const Model *model, const PreparedFilters *filters, const map<string, idx_t> &powers) {
	bool filtered = filters && !filters->filters.empty();
	if (!model) {
		if (filtered || !powers.empty()) {
			throw Unsupported();
		}
		return 1.0;
	}
	Powers columns;
	for (auto &entry : powers) {
		auto column = model->Find(entry.first);
		if (!column.IsValid()) {
			throw Unsupported();
		}
		columns[column.GetIndex()] = entry.second;
	}
	PreparedFilters none;
	none.types.assign(model->Columns().size(), LogicalType::DOUBLE);
	double result;
	if (!Expect(context, *model, filters ? *filters : none, columns, result)) {
		throw Unsupported();
	}
	return result;
}

//! Fanout powers and member weights of class c, except relation skip.
void JoinEstimator::Members(const Class &c, optional_idx skip, map<string, idx_t> &powers, double &scalar) {
	for (auto &fk : c.fks) {
		if (skip.IsValid() && fk.first == skip.GetIndex()) {
			continue;
		}
		powers[FanoutName(relations[fk.first].table.get().name, fk.second)]++;
		scalar *= Weight(fk.first, &c);
	}
}

//! Expected matches per row of r, for its subtree away from class parent.
double JoinEstimator::Weight(idx_t r, const Class *parent) {
	map<string, idx_t> powers;
	double scalar = 1.0;
	auto dom = domain_of.find(r);
	if (dom != domain_of.end() && dom->second != parent) {
		Members(*dom->second, optional_idx(), powers, scalar);
	}
	for (auto c : fk_of[r]) {
		if (c != parent) {
			scalar *= Average(*c, r);
		}
	}
	auto &relation = relations[r];
	return ExpectOn(relation.model.get(), &relation.filters, powers) * scalar;
}

const Model *JoinEstimator::DomainModel(const Class &c) {
	if (c.d.IsValid()) {
		return relations[c.d.GetIndex()].model.get();
	}
	auto hit = domain_models.find(c.id);
	if (hit == domain_models.end()) {
		auto table = KeyTable(context, domains[c.id].key);
		hit = domain_models.emplace(c.id, table ? GetModel(context, *table) : nullptr).first;
	}
	return hit->second.get();
}

idx_t JoinEstimator::DomainRows(const Class &c) {
	if (c.d.IsValid()) {
		return relations[c.d.GetIndex()].rows;
	}
	auto table = KeyTable(context, domains[c.id].key);
	if (!table || !table->IsDuckTable()) {
		throw Unsupported();
	}
	return table->GetStorage().GetTotalRows();
}

//! Expected matches through class c per row of r, a foreign key in c.
double JoinEstimator::Average(const Class &c, idx_t r) {
	map<string, idx_t> powers;
	double scalar = 1.0;
	Members(c, r, powers, scalar);
	powers[FanoutName(relations[r].table.get().name, c.fks.at(r))]++;

	const PreparedFilters *filters = nullptr;
	if (c.d.IsValid()) {
		auto d = c.d.GetIndex();
		filters = &relations[d].filters;
		for (auto other : fk_of[d]) {
			scalar *= Average(*other, d);
		}
	}
	auto e = ExpectOn(DomainModel(c), filters, powers);
	return double(DomainRows(c)) / double(MaxValue<idx_t>(relations[r].rows, 1)) * e * scalar;
}

} // namespace duckdb
