#include "planner.h"

#include "../catalog/catalog.h"
#include "sql_error.h"
#include "sql_semantics.h"

namespace flintdb {

namespace {

// Throws SqlSemanticError if `pred.column` is qualified with some table
// other than `table` (there is only one table in scope for a join-free
// PlanTableAccess call), or if its column doesn't exist on `table`, or if
// its literal's kind doesn't match that column's type. This is the
// single-table counterpart of ResolveColumnSide below, used wherever
// there's exactly one table in scope, so there's no ambiguity to resolve
// -- only existence and type to check.
void ValidatePredicateAgainstTable(const TableInfo& table, const Predicate& pred) {
    if (pred.column.table_name.has_value() && *pred.column.table_name != table.name) {
        throw SqlSemanticError("predicate column is qualified with table '" + *pred.column.table_name +
                                "', which is not in scope here (expected '" + table.name + "')");
    }
    const ColumnDef& col = RequireColumn(table, pred.column.column_name);
    RequireLiteralMatchesColumn(table, col, pred.value);
}

// The one index (PK or secondary) on `table_name`'s `column_name`, or
// nullptr if none exists. Because Catalog::CreateTable/CreateIndex both
// reject a TEXT column outright (docs/DECISIONS.md D-046), finding an
// index here is sufficient on its own to know `column_name` is INTEGER
// and its index is a real, usable BPlusTree -- no separate type check is
// needed at the call sites below.
const IndexInfo* FindIndexOnColumn(const Catalog& catalog, const std::string& table_name,
                                    const std::string& column_name) {
    for (const IndexInfo* idx : catalog.IndexesOnTable(table_name)) {
        if (idx->column_name == column_name) return idx;
    }
    return nullptr;
}

// Which of a JOIN's two tables an (unqualified or qualified) ColumnRef
// belongs to.
enum class JoinSide { kOuter, kInner };

// Resolves `col` against a JOIN's two tables (`outer` is always the FROM
// table, `inner` is always the JOIN table, docs/DECISIONS.md D-047):
//
//   - If `col.table_name` is set, it must name exactly one of `outer`/
//     `inner` (checked by name, since PlanSelect already rejects a
//     self-join before this is ever called, so outer.name != inner.name
//     here always) and that table must actually have the column.
//   - If unset, `col.column_name` must exist on exactly one of the two
//     tables -- existing on both is ambiguous, existing on neither is
//     unknown.
//
// Throws SqlSemanticError for every failure mode above.
JoinSide ResolveColumnSide(const TableInfo& outer, const TableInfo& inner, const ColumnRef& col) {
    if (col.table_name.has_value()) {
        if (*col.table_name == outer.name) {
            RequireColumn(outer, col.column_name);
            return JoinSide::kOuter;
        }
        if (*col.table_name == inner.name) {
            RequireColumn(inner, col.column_name);
            return JoinSide::kInner;
        }
        throw SqlSemanticError("unknown table '" + *col.table_name + "' in query (expected '" + outer.name +
                                "' or '" + inner.name + "')");
    }
    const ColumnDef* in_outer = outer.FindColumn(col.column_name);
    const ColumnDef* in_inner = inner.FindColumn(col.column_name);
    if (in_outer != nullptr && in_inner != nullptr) {
        throw SqlSemanticError("column '" + col.column_name + "' is ambiguous -- it exists in both '" + outer.name +
                                "' and '" + inner.name + "'; qualify it as table.column");
    }
    if (in_outer != nullptr) return JoinSide::kOuter;
    if (in_inner != nullptr) return JoinSide::kInner;
    throw SqlSemanticError("column '" + col.column_name + "' does not exist in '" + outer.name + "' or '" +
                            inner.name + "'");
}

}  // namespace

TableAccessPlan PlanTableAccess(const Catalog& catalog, const std::string& table_name,
                                 const std::vector<Predicate>& predicates) {
    const TableInfo* table = catalog.FindTable(table_name);
    if (table == nullptr) {
        throw SqlSemanticError("no such table '" + table_name + "'");
    }
    for (const Predicate& pred : predicates) {
        ValidatePredicateAgainstTable(*table, pred);
    }

    TableAccessPlan plan;
    plan.table_name = table_name;
    plan.method = ScanMethod::kSeqScan;

    // Fixed rule (docs/SPEC.md section 4, docs/DECISIONS.md D-047): an
    // index scan iff some predicate is an equality against an indexed
    // column. No cost model exists to choose among several eligible
    // predicates, so the first one in WHERE-clause order wins,
    // deterministically.
    std::optional<size_t> driving_predicate;
    for (size_t i = 0; i < predicates.size(); i++) {
        const Predicate& pred = predicates[i];
        if (pred.op != Comparator::kEq) continue;
        const IndexInfo* index = FindIndexOnColumn(catalog, table_name, pred.column.column_name);
        if (index == nullptr) continue;
        driving_predicate = i;
        plan.method = ScanMethod::kIndexScan;
        plan.index_name = index->name;
        // D-046 guarantees this column -- and therefore this predicate's
        // already-validated literal -- is INTEGER, so int_value is safe
        // to read directly.
        plan.index_key = pred.value.int_value;
        break;
    }

    // Every predicate becomes a residual filter except the one that drove
    // the index probe (if any) -- re-checking that one is pure waste,
    // since a B+-tree key match is already exact (D-047).
    for (size_t i = 0; i < predicates.size(); i++) {
        if (driving_predicate.has_value() && i == *driving_predicate) continue;
        plan.residual_predicates.push_back(predicates[i]);
    }
    return plan;
}

SelectPlan PlanSelect(const Catalog& catalog, const SelectStatement& stmt) {
    const TableInfo* from_table = catalog.FindTable(stmt.from_table);
    if (from_table == nullptr) {
        throw SqlSemanticError("no such table '" + stmt.from_table + "'");
    }

    SelectPlan plan;
    plan.select_star = stmt.select_star;

    if (!stmt.join.has_value()) {
        // No JOIN: resolve the select list against the one table in
        // scope, and hand WHERE planning entirely to PlanTableAccess.
        if (!stmt.select_star) {
            for (const ColumnRef& col : stmt.select_list) {
                if (col.table_name.has_value() && *col.table_name != from_table->name) {
                    throw SqlSemanticError("unknown table '" + *col.table_name + "' in select list");
                }
                RequireColumn(*from_table, col.column_name);
                plan.select_list.push_back(ColumnRef{from_table->name, col.column_name});
            }
        }
        plan.access = PlanTableAccess(catalog, stmt.from_table, stmt.where_predicates);
        return plan;
    }

    // JOIN path: outer is always the FROM table, inner is always the
    // JOIN table (D-047 -- docs/SPEC.md section 4 rules out reordering).
    const JoinClause& join = *stmt.join;
    const TableInfo* inner_table = catalog.FindTable(join.table_name);
    if (inner_table == nullptr) {
        throw SqlSemanticError("no such table '" + join.table_name + "'");
    }
    if (inner_table->name == from_table->name) {
        // D-047: a self-join is structurally unusable in this grammar --
        // there's no AS-aliasing syntax to address the two occurrences of
        // the same table separately, so every reference to it would
        // silently resolve to just one of them. Rejecting this plainly is
        // better than leaving the other occurrence permanently
        // unreachable.
        throw SqlSemanticError("JOIN requires two distinct tables (got '" + from_table->name +
                                "' twice) -- self-joins aren't supported, since this grammar has no table "
                                "aliasing to distinguish the two occurrences");
    }

    JoinSide left_side = ResolveColumnSide(*from_table, *inner_table, join.left_column);
    JoinSide right_side = ResolveColumnSide(*from_table, *inner_table, join.right_column);
    if (left_side == right_side) {
        throw SqlSemanticError("JOIN ON clause must compare one column from '" + from_table->name + "' to one "
                                "column from '" + inner_table->name + "', not two columns of the same table");
    }
    const std::string& outer_join_column =
        (left_side == JoinSide::kOuter) ? join.left_column.column_name : join.right_column.column_name;
    const std::string& inner_join_column =
        (left_side == JoinSide::kInner) ? join.left_column.column_name : join.right_column.column_name;

    const ColumnDef& outer_join_col = RequireColumn(*from_table, outer_join_column);
    const ColumnDef& inner_join_col = RequireColumn(*inner_table, inner_join_column);
    if (outer_join_col.type != inner_join_col.type) {
        throw SqlSemanticError("JOIN ON compares '" + from_table->name + "." + outer_join_column + "' (" +
                                ColumnTypeToString(outer_join_col.type) + ") to '" + inner_table->name + "." +
                                inner_join_column + "' (" + ColumnTypeToString(inner_join_col.type) +
                                ") -- both sides of an equi-join must be the same type");
    }

    // Split WHERE predicates by which table they reference, validating
    // each one's column existence and literal type as we go (the same
    // checks ValidatePredicateAgainstTable does for the join-free path,
    // just resolved against whichever of the two tables this predicate
    // actually names).
    std::vector<Predicate> outer_predicates;
    std::vector<Predicate> inner_predicates;
    for (const Predicate& pred : stmt.where_predicates) {
        JoinSide side = ResolveColumnSide(*from_table, *inner_table, pred.column);
        const TableInfo& side_table = (side == JoinSide::kOuter) ? *from_table : *inner_table;
        const ColumnDef& col = RequireColumn(side_table, pred.column.column_name);
        RequireLiteralMatchesColumn(side_table, col, pred.value);
        (side == JoinSide::kOuter ? outer_predicates : inner_predicates).push_back(pred);
    }

    JoinPlan jplan;
    // Re-validated (harmlessly redundantly) by PlanTableAccess itself --
    // keeping PlanTableAccess independently correct, regardless of caller,
    // is worth the small amount of repeated work.
    jplan.outer = PlanTableAccess(catalog, from_table->name, outer_predicates);
    jplan.inner_table_name = inner_table->name;
    jplan.inner_residual_predicates = std::move(inner_predicates);
    jplan.outer_join_column = outer_join_column;
    jplan.inner_join_column = inner_join_column;

    // D-047: the inner side's access method is chosen solely by whether
    // an index exists on the join column -- never by any of its own
    // WHERE predicates (inner_residual_predicates above), since there's
    // no cost model to arbitrate between the two.
    const IndexInfo* inner_index = FindIndexOnColumn(catalog, inner_table->name, inner_join_column);
    if (inner_index != nullptr) {
        jplan.inner_method = ScanMethod::kIndexScan;
        jplan.inner_index_name = inner_index->name;
    } else {
        jplan.inner_method = ScanMethod::kSeqScan;
    }

    if (!stmt.select_star) {
        for (const ColumnRef& col : stmt.select_list) {
            JoinSide side = ResolveColumnSide(*from_table, *inner_table, col);
            const TableInfo& side_table = (side == JoinSide::kOuter) ? *from_table : *inner_table;
            plan.select_list.push_back(ColumnRef{side_table.name, col.column_name});
        }
    }

    plan.access = std::move(jplan);
    return plan;
}

}  // namespace flintdb
