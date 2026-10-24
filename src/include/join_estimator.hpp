//===----------------------------------------------------------------------===//
// Join sizes from table models with fanout columns (BayesCard).
//
// The relations of a set and the classes of columns joining them must form
// a tree. Each class has a domain: the table whose primary key it is, in the
// set or not. e.g. cast_info ⋈ movie_info on movie_id goes through title:
//
//    σT ──── title.id = ci.movie_id = mi.movie_id ──── σci
//                             │
//                            σmi
//
//    card = |T| · E_T[1(f_T) · F_ci · F_mi] · P_ci(f_ci) · P_mi(f_mi)
//
// For any such tree, rooted at a relation:
//
//    weight(R) = E_R[1(f_R) · Π F_M] · Π weight(M) · Π avg(c, R)
//                (M: other members of the class R is the domain of;
//                 c: the classes R is a foreign key in)
//    avg(c, R) = |D| / |R| · E_D[1(f_D) · F_R · Π F_M] · Π weight(M) · ...
//    card      = |root| · weight(root)
//
// Only rows whose foreign key is not NULL join, and filters can pick them out:
// e.g. votes with a BountyAmount almost all have a UserId, most votes do not.
// So a foreign-key relation's filters are taken given its keys are not NULL:
// E[1(f) ...] becomes E[1(f ∧ fk NOT NULL) ...] / P(fk NOT NULL).
//
// A filtered dimension D, small enough to read, is folded into its foreign key
// R when it joins nothing else in the set: the keys passing D's filters become
// a filter on R, R.fk IN (keys), which R's model evaluates jointly with R's
// own filters. Each such row of R matches one row of D. e.g. info_type with
// info = 'genres' and movie_info with info IN ('Horror', ...): the genres only
// occur with their info type, which independent filters would miss.
//===----------------------------------------------------------------------===//

#pragma once

#include "bn_estimator.hpp"
#include "keys.hpp"
#include "query_graph.hpp"

namespace duckdb {
namespace cardinal {

//! A relation of the query, ready for estimates.
struct JoinRelation {
	reference<TableCatalogEntry> table;
	//! Null: the table has no model, and must have no filter.
	shared_ptr<Model> model;
	PreparedFilters filters;
	//! Rows of the table, before filters.
	idx_t rows;
};

//! A class of the query graph: its primary key, and the table that key is in.
struct JoinDomain {
	//! The relation holding the primary key, if it is in the query.
	optional_idx relation;
	KeyColumn key;
	//! Invalid: the class has no known primary key, and cannot be estimated.
	bool known = false;
	//! Keys of the domain relation that pass its filters, when it is a small dimension.
	bool folds = false;
	vector<Value> keys;
	//! The domain relation is small enough to read its keys.
	bool small = false;
};

class JoinEstimator {
public:
	JoinEstimator(ClientContext &context, const QueryGraph &graph, vector<JoinRelation> relations,
	              vector<JoinDomain> domains);

	//! Rows of a connected set of relations (bit i: relation i). False outside the tree shape.
	bool Estimate(uint64_t set, double &card);

private:
	//! Conditions by relation: a branch of a filter across relations.
	using Branch = map<idx_t, vector<const Expression *>>;

	bool EstimateWith(uint64_t set, const Branch &conditions, double &card);
	bool Split(const Expression &expr, vector<Branch> &result);
	GraphColumn ColumnOf(const Expression &expr);
	bool Override(idx_t r, const vector<const Expression *> &conditions);
	//! A relation's filters, with the conditions of the current term.
	const PreparedFilters &BaseFilters(idx_t r);

	//! A class restricted to the set: domain relation, if in the set, and foreign keys.
	struct Class {
		idx_t id;
		optional_idx d;
		//! Foreign-key relation -> its column in the class.
		map<idx_t, string> fks;
		//! No key: its relations join through their values, e.g. d1.week = d2.week.
		bool values = false;
	};

	double Weight(idx_t r, const Class *parent);
	double Average(const Class &c, idx_t r);
	void Members(const Class &c, optional_idx skip, map<string, idx_t> &powers, double &scalar);
	double ExpectOn(const Model *model, const PreparedFilters *filters, const map<string, idx_t> &powers,
	                const StateWeights *state_weights = nullptr);
	vector<double> ExpectByStateOn(const Model *model, const PreparedFilters *filters,
	                               const map<string, idx_t> &powers, idx_t column, const StateWeights *state_weights);
	//! Weights on r's columns from its value classes except parent: the rows joining each value.
	StateWeights ValueWeights(idx_t r, const Class *parent);
	//! Rows of m's subtree away from class c, by state of m's column in c.
	vector<double> Mass(idx_t m, const Class &c);
	//! Masses by state of one column, moved onto the states of another column through their values.
	vector<double> MapStates(const vector<double> &mass, const ModelColumn &from, const ModelColumn &to);
	vector<double> RelationExpectByState(idx_t r, const map<string, idx_t> &powers, idx_t column,
	                                     const StateWeights *state_weights);
	const Model *DomainModel(const Class &c);
	idx_t DomainRows(const Class &c);

	ClientContext &context;
	const QueryGraph &graph;
	vector<JoinRelation> relations;
	vector<JoinDomain> domains;
	//! Relation of each scan, and class of each joined column.
	unordered_map<TableIndex, idx_t> relation_of;
	map<GraphColumn, idx_t> class_of;
	//! Branches of each filter across relations. A branch on relation INVALID_INDEX: out of reach.
	vector<vector<Branch>> filter_branches;
	//! Relations' filters with a term's conditions: current ones, and all built.
	map<idx_t, const PreparedFilters *> overrides;
	Branch override_conditions;
	map<string, PreparedFilters> override_filters;
	//! Keys of a small dimension that pass its filters and a term's conditions, by filters key.
	map<string, unique_ptr<vector<Value>>> override_keys;
	//! The keys a folded class's dimension passes, null when it cannot fold.
	const vector<Value> *FoldKeys(const Class &c);

	//! Models of domain tables outside the query.
	map<idx_t, shared_ptr<Model>> domain_models;
	//! Rows of domain tables outside the query, by class. Invalid: not a DuckDB table.
	map<idx_t, optional_idx> domain_rows;

	//! A relation's filters with the keys of the dimensions folded into it, if any.
	const PreparedFilters &FiltersOf(idx_t r);
	//! E_r[1(f_r) · Π F^k], given r's foreign keys in the set are not NULL.
	double RelationExpect(idx_t r, const map<string, idx_t> &powers, const StateWeights *state_weights = nullptr);

	//! The set being estimated.
	vector<Class> classes;
	map<idx_t, const Class *> domain_of;
	map<idx_t, vector<const Class *>> fk_of;
	//! Value classes of each relation.
	map<idx_t, vector<const Class *>> value_of;
	//! Folded classes, and the filters of their foreign-key relations.
	set<idx_t> folded;
	map<idx_t, const PreparedFilters *> folded_filters;
	//! Relations with foreign keys in the set: their filters with fk IS NOT NULL, and the
	//! IS NOT NULL predicates alone.
	map<idx_t, const PreparedFilters *> keyed_filters;
	map<idx_t, const PreparedFilters *> key_filters;

	//! A relation's filters for some folded classes and foreign keys.
	struct RelationFilters {
		//! A folded dimension passes no key.
		bool empty = false;
		bool has_folded = false;
		PreparedFilters folded;
		bool has_keys = false;
		PreparedFilters keyed;
		PreparedFilters key;
	};
	RelationFilters BuildFilters(idx_t r, const vector<const Class *> &folds, const vector<const Class *> &fks);
	//! Built filters, shared by sets, e.g. "2|f3,|k3,5,".
	map<string, RelationFilters> relation_filters;
};

} // namespace cardinal
} // namespace duckdb
