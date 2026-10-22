#include "join_estimator.hpp"
#include "models.hpp"

#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {
namespace cardinal {

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
			members[column.first] = relations[column.first].table.get().GetColumns().GetColumn(LogicalIndex(column.second)).Name().GetIdentifierName();
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

	// Fold small filtered dimensions that join only their foreign key in the set.
	folded.clear();
	duckdb::set<idx_t> dimensions;
	map<idx_t, vector<const Class *>> folds;
	for (auto &c : classes) {
		auto &domain = domains[c.id];
		if (!domain.folds || !c.d.IsValid() || c.fks.size() != 1 || !fk_of[c.d.GetIndex()].empty()) {
			continue;
		}
		auto r = c.fks.begin()->first;
		auto &relation = relations[r];
		if (!relation.model || !relation.model->Find(c.fks.begin()->second).IsValid()) {
			continue;
		}
		folds[r].push_back(&c);
		folded.insert(c.id);
		dimensions.insert(c.d.GetIndex());
	}

	// Each relation's filters for its folded classes and foreign keys: sets share them.
	folded_filters.clear();
	keyed_filters.clear();
	key_filters.clear();
	duckdb::set<idx_t> filtered;
	for (auto &entry : folds) {
		filtered.insert(entry.first);
	}
	for (auto &entry : fk_of) {
		filtered.insert(entry.first);
	}
	for (auto r : filtered) {
		if (!relations[r].model) {
			continue;
		}
		// e.g. "2|f3,|k3,5," for relation 2 with class 3 folded, foreign key in classes 3 and 5.
		auto key = to_string(r) + "|f";
		for (auto c : folds[r]) {
			key += to_string(c->id) + ",";
		}
		key += "|k";
		for (auto c : fk_of[r]) {
			key += to_string(c->id) + ",";
		}
		auto hit = relation_filters.find(key);
		if (hit == relation_filters.end()) {
			hit = relation_filters.emplace(key, BuildFilters(r, folds[r], fk_of[r])).first;
		}
		auto &built = hit->second;
		if (built.empty) {
			// A folded dimension passes no key: no row of r joins.
			card = 0;
			return true;
		}
		if (built.has_folded) {
			folded_filters[r] = &built.folded;
		}
		if (built.has_keys) {
			keyed_filters[r] = &built.keyed;
			key_filters[r] = &built.key;
		}
	}

	// The root is never a folded dimension: its foreign key accounts for it.
	idx_t root = 0;
	while (!Has(set, root) || dimensions.count(root)) {
		root++;
	}
	try {
		card = double(relations[root].rows) * Weight(root, nullptr);
	} catch (Unsupported &) {
		return false;
	}
	return true;
}

JoinEstimator::RelationFilters JoinEstimator::BuildFilters(idx_t r, const vector<const Class *> &folds,
                                                          const vector<const Class *> &fks) {
	RelationFilters result;
	auto &relation = relations[r];
	auto &model = *relation.model;
	auto types = relation.filters.types;
	if (types.empty()) {
		types.assign(model.Columns().size(), LogicalType::DOUBLE);
	}

	// R.fk IN (keys of each folded dimension), with R's own filters.
	auto &with_keys = result.folded;
	with_keys.types = types;
	for (auto &expr : relation.filters.filters) {
		with_keys.filters.push_back(expr->Copy());
	}
	with_keys.key = relation.filters.key;
	for (auto c : folds) {
		auto column = model.Find(c->fks.at(r)).GetIndex();
		auto &type = model.Columns()[column].type;
		auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
		in->GetChildrenMutable().push_back(make_uniq<BoundReferenceExpression>(type, column));
		string keys;
		for (auto &key : domains[c->id].keys) {
			Value value = key;
			if (!value.DefaultTryCastAs(type)) {
				continue;
			}
			keys += value.ToString() + ",";
			in->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(value));
		}
		if (in->GetChildren().size() == 1) {
			result.empty = true;
			return result;
		}
		with_keys.key += " AND #" + to_string(column) + " IN (" + keys + ")";
		with_keys.filters.push_back(std::move(in));
	}
	result.has_folded = !folds.empty();
	auto &base = result.has_folded ? result.folded : relation.filters;

	// Foreign keys not NULL: with the filters, and alone.
	result.keyed.types = types;
	result.key.types = types;
	for (auto &expr : base.filters) {
		result.keyed.filters.push_back(expr->Copy());
	}
	result.keyed.key = base.key;
	for (auto c : fks) {
		auto column = model.Find(c->fks.at(r));
		if (!column.IsValid()) {
			continue;
		}
		auto &type = model.Columns()[column.GetIndex()].type;
		for (auto target : {&result.keyed, &result.key}) {
			auto not_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			not_null->GetChildrenMutable().push_back(make_uniq<BoundReferenceExpression>(type, column.GetIndex()));
			target->filters.push_back(std::move(not_null));
			target->key += " AND #" + to_string(column.GetIndex()) + " IS NOT NULL";
		}
	}
	result.has_keys = !result.key.filters.empty();
	return result;
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
	// No filters: no predicate reads the types.
	static const PreparedFilters none;
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
		powers[FanoutName(relations[fk.first].table.get().name.GetIdentifierName(), fk.second)]++;
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
	return RelationExpect(r, powers) * scalar;
}

double JoinEstimator::RelationExpect(idx_t r, const map<string, idx_t> &powers) {
	auto keyed = keyed_filters.find(r);
	if (keyed == keyed_filters.end()) {
		return ExpectOn(relations[r].model.get(), &FiltersOf(r), powers);
	}
	auto keys = ExpectOn(relations[r].model.get(), key_filters[r], {});
	if (keys <= 0) {
		return 0;
	}
	return ExpectOn(relations[r].model.get(), keyed->second, powers) / keys;
}

const PreparedFilters &JoinEstimator::FiltersOf(idx_t r) {
	auto hit = folded_filters.find(r);
	return hit != folded_filters.end() ? *hit->second : relations[r].filters;
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
	// A table outside the query: looked up once.
	auto hit = domain_rows.find(c.id);
	if (hit == domain_rows.end()) {
		auto table = KeyTable(context, domains[c.id].key);
		optional_idx rows;
		if (table && table->IsDuckTable()) {
			rows = table->GetStorage().GetTotalRows();
		}
		hit = domain_rows.emplace(c.id, rows).first;
	}
	if (!hit->second.IsValid()) {
		throw Unsupported();
	}
	return hit->second.GetIndex();
}

//! Expected matches through class c per row of r, a foreign key in c.
double JoinEstimator::Average(const Class &c, idx_t r) {
	// Folded: r's filter keeps the rows whose key passes, each matching one row.
	if (folded.count(c.id)) {
		return 1.0;
	}
	map<string, idx_t> powers;
	double scalar = 1.0;
	Members(c, r, powers, scalar);
	powers[FanoutName(relations[r].table.get().name.GetIdentifierName(), c.fks.at(r))]++;

	double e;
	if (c.d.IsValid()) {
		auto d = c.d.GetIndex();
		for (auto other : fk_of[d]) {
			scalar *= Average(*other, d);
		}
		e = RelationExpect(d, powers);
	} else {
		e = ExpectOn(DomainModel(c), nullptr, powers);
	}
	return double(DomainRows(c)) / double(MaxValue<idx_t>(relations[r].rows, 1)) * e * scalar;
}

} // namespace cardinal
} // namespace duckdb
