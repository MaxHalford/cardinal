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
//===----------------------------------------------------------------------===//

#pragma once

#include "bn_estimator.hpp"
#include "keys.hpp"
#include "query_graph.hpp"

namespace duckdb {

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
};

class JoinEstimator {
public:
	JoinEstimator(ClientContext &context, const QueryGraph &graph, vector<JoinRelation> relations,
	              vector<JoinDomain> domains);

	//! Rows of a connected set of relations (bit i: relation i). False outside the tree shape.
	bool Estimate(uint64_t set, double &card);

private:
	//! A class restricted to the set: domain relation, if in the set, and foreign keys.
	struct Class {
		idx_t id;
		optional_idx d;
		//! Foreign-key relation -> its column in the class.
		map<idx_t, string> fks;
	};

	double Weight(idx_t r, const Class *parent);
	double Average(const Class &c, idx_t r);
	void Members(const Class &c, optional_idx skip, map<string, idx_t> &powers, double &scalar);
	double ExpectOn(const Model *model, const PreparedFilters *filters, const map<string, idx_t> &powers);
	const Model *DomainModel(const Class &c);
	idx_t DomainRows(const Class &c);

	ClientContext &context;
	const QueryGraph &graph;
	vector<JoinRelation> relations;
	vector<JoinDomain> domains;
	//! Models of domain tables outside the query.
	map<idx_t, shared_ptr<Model>> domain_models;

	//! The set being estimated.
	vector<Class> classes;
	map<idx_t, const Class *> domain_of;
	map<idx_t, vector<const Class *>> fk_of;
};

} // namespace duckdb
