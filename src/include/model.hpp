//===----------------------------------------------------------------------===//
// A table model: a Bayesian network shaped as a tree (Chow-Liu).
//
//            CreationDate             P(CreationDate)
//                 |
//             Reputation              P(Reputation | CreationDate)
//              /      \
//        UpVotes    F(badges.UserId)  P(UpVotes | Reputation), ...
//
// Each column has a few states: its frequent values, plus NULL, plus OTHER
// for the infrequent values grouped together (compacted columns).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

enum class StateKind { VALUE, NULL_VALUE, OTHER };

//! One P(state | parent state) entry. Unobserved pairs have probability 0.
struct Probability {
	idx_t state;
	idx_t parent_state;
	double p;
};

//! Values of one column as chunks, for DuckDB's expression executor.
using Chunks = vector<unique_ptr<DataChunk>>;

struct ModelColumn {
	string name;
	//! The table column's type. VARCHAR for columns that are not table columns.
	LogicalType type = LogicalType::VARCHAR;
	optional_idx parent;
	vector<idx_t> children;

	//! States, as values of the table column's type. NULL for non-value states.
	vector<Value> values;
	vector<StateKind> kinds;
	//! Infrequent values grouped into OTHER. Invalid for columns not compacted.
	optional_idx members;

	vector<Probability> probabilities;

	//! values as chunks: built with the model, read by estimates.
	Chunks state_chunks;

	idx_t FrequentCount() const;
};

//! The model column counting the rows of fk_table that reference each row,
//! e.g. "F(cast_info.movie_id)". Its states are bins, valued by their mean count.
string FanoutName(const string &fk_table, const string &fk_column);

//! Per-state weights of one column, e.g. 1 when a state passes a filter, else 0.
//! Empty: no evidence on the column.
using Weights = vector<vector<double>>;

class Model {
public:
	Model(idx_t rows, vector<ModelColumn> columns);

	idx_t Rows() const;
	const vector<ModelColumn> &Columns() const;
	optional_idx Find(const string &column) const;

	//! E[Π weights]: e.g. the share of rows that pass filters, with 0/1 weights.
	double Expectation(const Weights &weights) const;

	//! Selectivities already estimated, by filter text. Planning asks again and again.
	bool FindSelectivity(const string &filters, double &selectivity) const;
	void AddSelectivity(const string &filters, double selectivity) const;

private:
	vector<double> Belief(idx_t column, const Weights &weights, const vector<bool> &evidence) const;

	idx_t rows;
	vector<ModelColumn> columns;
	vector<idx_t> roots;
	//! Parents before children.
	vector<idx_t> order;
	case_insensitive_map_t<idx_t> by_name;

	mutable mutex selectivity_lock;
	mutable unordered_map<string, double> selectivities;
};

} // namespace duckdb
