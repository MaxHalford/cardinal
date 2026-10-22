#include "query_graph.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <bitset>

namespace duckdb {
namespace cardinal {

//! Walks the analyzed plan bottom-up, resolving column bindings to relation columns.
class GraphBuilder {
public:
	explicit GraphBuilder(QueryGraph &graph) : graph(graph) {
	}

	void Visit(LogicalOperator &op) {
		for (auto &child : op.children) {
			Visit(*child);
		}
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_GET:
			AddRelation(op.Cast<LogicalGet>());
			break;
		case LogicalOperatorType::LOGICAL_PROJECTION:
			MapProjection(op.Cast<LogicalProjection>());
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			AddFilter(op.Cast<LogicalFilter>());
			break;
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
			AddJoin(op.Cast<LogicalComparisonJoin>());
			break;
		case LogicalOperatorType::LOGICAL_ANY_JOIN: {
			// e.g. an OR across both sides, as the join's condition.
			auto &join = op.Cast<LogicalAnyJoin>();
			if (join.join_type == JoinType::INNER) {
				AddCrossFilter(*join.condition);
			}
			break;
		}
		default:
			break;
		}
	}

private:
	void AddRelation(LogicalGet &get) {
		if (!get.GetTable() || !get.children.empty()) {
			return;
		}
		auto relation = graph.relations.size();
		graph.relations.push_back(GraphRelation {get, {}});
		relation_of[get.table_index] = relation;

		auto &ids = get.GetColumnIds();
		for (idx_t i = 0; i < ids.size(); i++) {
			if (ids[i].IsVirtualColumn()) {
				continue;
			}
			columns[ColumnBinding(get.table_index, ProjectionIndex(i))] = GraphColumn(relation, ids[i].GetPrimaryIndex());
			scan_binding[GraphColumn(relation, ids[i].GetPrimaryIndex())] = ColumnBinding(get.table_index, ProjectionIndex(i));
		}
	}

	//! A projection passes columns through under new bindings.
	void MapProjection(LogicalProjection &projection) {
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			auto column = Resolve(*projection.expressions[i]);
			if (column.first != DConstants::INVALID_INDEX) {
				columns[ColumnBinding(projection.table_index, ProjectionIndex(i))] = column;
			}
		}
	}

	//! Filters on one scan are its filters. Equalities across scans are join conditions.
	void AddFilter(LogicalFilter &filter) {
		auto &child = *filter.children[0];
		bool on_scan = child.type == LogicalOperatorType::LOGICAL_GET && child.Cast<LogicalGet>().GetTable();
		for (auto &expr : filter.expressions) {
			if (AddEquality(*expr)) {
				continue;
			}
			if (on_scan) {
				auto relation = relation_of.find(child.Cast<LogicalGet>().table_index);
				if (relation != relation_of.end()) {
					graph.relations[relation->second].filters.push_back(expr->Copy());
				}
				continue;
			}
			AddCrossFilter(*expr);
		}
	}

	//! A filter above joins, with its column references traced to the scans. On one relation:
	//! one of its filters. On several: a filter of the graph.
	void AddCrossFilter(const Expression &expr) {
		auto copy = expr.Copy();
		uint64_t relations = 0;
		bool resolved = true;
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    copy, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &slot) {
			    auto column = columns.find(ref.Binding());
			    auto scan = column == columns.end() ? scan_binding.end() : scan_binding.find(column->second);
			    // Relations are bits of a 64-bit set.
			    if (scan == scan_binding.end() || column->second.first >= 64) {
				    resolved = false;
				    return;
			    }
			    relations |= uint64_t(1) << column->second.first;
			    // Named, as the scan's own column: e.g. to read a dimension's passing keys with SQL.
			    auto &get = graph.relations[column->second.first].get.get();
			    slot = make_uniq<BoundColumnRefExpression>(get.names[column->second.second], ref.GetReturnType(),
			                                               scan->second);
		    });
		if (!resolved) {
			// Its relations are unknown: no join estimate can account for it.
			graph.filters.push_back(GraphFilter {std::move(copy), ~uint64_t(0), false, false});
			return;
		}
		auto count = std::bitset<64>(relations).count();
		if (count == 0) {
			return;
		}
		if (count == 1) {
			graph.relations[CountZeros<uint64_t>::Trailing(relations)].filters.push_back(std::move(copy));
			return;
		}
		bool is_or = copy->GetExpressionType() == ExpressionType::CONJUNCTION_OR;
		graph.filters.push_back(GraphFilter {std::move(copy), relations, true, is_or});
	}

	void AddJoin(LogicalComparisonJoin &join) {
		if (join.join_type != JoinType::INNER) {
			return;
		}
		for (auto &condition : join.conditions) {
			// e.g. an OR across both sides, pushed into the join.
			if (!condition.IsComparison()) {
				AddCrossFilter(condition.GetJoinExpression());
				continue;
			}
			if (condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
			    AddEdge(condition.GetLHS(), condition.GetRHS())) {
				continue;
			}
			// e.g. a.x < b.y: a filter across the relations.
			auto comparison = BoundComparisonExpression::Create(condition.GetComparisonType(), condition.GetLHS().Copy(),
			                                                    condition.GetRHS().Copy());
			AddCrossFilter(*comparison);
		}
	}

	bool AddEquality(Expression &expr) {
		if (!BoundComparisonExpression::IsComparison(expr) || expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		return AddEdge(BoundComparisonExpression::Left(comparison), BoundComparisonExpression::Right(comparison));
	}

	bool AddEdge(const Expression &left, const Expression &right) {
		auto a = Resolve(left), b = Resolve(right);
		if (a.first == DConstants::INVALID_INDEX || b.first == DConstants::INVALID_INDEX || a.first == b.first) {
			return false;
		}
		// Pushdown repeats conditions across joins: one edge per column pair, as DuckDB.
		auto key = a < b ? make_pair(a, b) : make_pair(b, a);
		if (seen.insert(key).second) {
			graph.edges.push_back(GraphEdge {key.first, key.second});
		}
		return true;
	}

	//! A column reference, possibly cast -> its relation column. Invalid for anything else.
	//! e.g. CAST(t.id AS DOUBLE) = ci.person_role_id joins t.id, as for DuckDB.
	GraphColumn Resolve(const Expression &expr_p) {
		reference<const Expression> expr(expr_p);
		while (BoundCastExpression::IsCast(expr.get())) {
			expr = BoundCastExpression::Child(expr.get().Cast<BoundFunctionExpression>());
		}
		if (expr.get().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return GraphColumn(DConstants::INVALID_INDEX, DConstants::INVALID_INDEX);
		}
		auto hit = columns.find(expr.get().Cast<BoundColumnRefExpression>().Binding());
		if (hit == columns.end()) {
			return GraphColumn(DConstants::INVALID_INDEX, DConstants::INVALID_INDEX);
		}
		return hit->second;
	}

	QueryGraph &graph;
	unordered_map<TableIndex, idx_t> relation_of;
	column_binding_map_t<GraphColumn> columns;
	//! The scan's binding of each relation column, for filters above joins.
	map<GraphColumn, ColumnBinding> scan_binding;
	set<pair<GraphColumn, GraphColumn>> seen;
};

//! Union-find over the edges' columns.
static void BuildClasses(QueryGraph &graph) {
	map<GraphColumn, GraphColumn> parent;
	std::function<GraphColumn(GraphColumn)> find = [&](GraphColumn x) {
		if (parent.find(x) == parent.end()) {
			parent[x] = x;
		}
		while (parent[x] != x) {
			x = parent[x];
		}
		return x;
	};
	for (auto &edge : graph.edges) {
		parent[find(edge.left)] = find(edge.right);
	}

	map<GraphColumn, idx_t> class_of;
	for (auto &entry : parent) {
		auto root = find(entry.first);
		if (class_of.find(root) == class_of.end()) {
			class_of[root] = graph.classes.size();
			graph.classes.emplace_back();
		}
		graph.classes[class_of[root]].push_back(entry.first);
	}
	for (auto &edge : graph.edges) {
		graph.edge_class.push_back(class_of[find(edge.left)]);
	}
}

QueryGraph BuildGraph(Optimizer &optimizer, LogicalOperator &plan) {
	QueryGraph graph;
	try {
		graph.plan = plan.Copy(optimizer.context);
	} catch (std::exception &) {
		return graph;
	}

	// The passes before join ordering that place filters and join conditions, in DuckDB's
	// order. The rewriter, e.g., takes a join condition repeated in each branch of an OR
	// out of it, and each table's part of the OR.
	optimizer.rewriter.VisitOperator(*graph.plan);
	FilterPullup pullup;
	graph.plan = pullup.Rewrite(std::move(graph.plan));
	FilterPushdown pushdown(optimizer);
	graph.plan = pushdown.Rewrite(std::move(graph.plan));

	GraphBuilder builder(graph);
	builder.Visit(*graph.plan);
	BuildClasses(graph);
	return graph;
}

} // namespace cardinal
} // namespace duckdb
