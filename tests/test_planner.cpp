// Tests for the planner (src/sql/planner.h/.cpp, task #42): the fixed
// index-vs-scan rule (docs/DECISIONS.md D-047) for PlanTableAccess, and
// SELECT/JOIN planning (equi-join with outer=FROM/inner=JOIN, select_list
// resolution, and the semantic errors both throw) for PlanSelect.
//
// Predicates are obtained by parsing a real SELECT statement and pulling
// its where_predicates out, rather than hand-building Predicate structs --
// this doubles as a light integration check that the parser's output
// feeds the planner correctly, and reads closer to what an end-to-end
// caller would actually do.
#include "../src/catalog/catalog.h"
#include "../src/sql/parser.h"
#include "../src/sql/planner.h"
#include "../src/sql/sql_error.h"
#include "test_framework.h"
#include "test_utils.h"

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {

std::vector<Predicate> WherePredicatesOf(const std::string& select_sql) {
    Statement stmt = Parse(select_sql);
    return std::get<SelectStatement>(stmt).where_predicates;
}

SelectStatement ParseSelect(const std::string& sql) {
    return std::get<SelectStatement>(Parse(sql));
}

}  // namespace

// ---------------------------------------------------------------------
// PlanTableAccess: single-table index-vs-scan.
// ---------------------------------------------------------------------

FLINTDB_TEST(planner_no_predicates_is_a_sequential_scan_with_no_residuals) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    TableAccessPlan plan = PlanTableAccess(cat, "widgets", {});
    FLINTDB_CHECK(plan.method == ScanMethod::kSeqScan);
    FLINTDB_CHECK(plan.residual_predicates.empty());
}

FLINTDB_TEST(planner_equality_on_primary_key_column_drives_an_index_scan) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    TableAccessPlan plan =
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE id = 5"));
    FLINTDB_CHECK(plan.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(plan.index_name, cat.PrimaryKeyIndex("widgets")->name);
    FLINTDB_CHECK_EQ(plan.index_key, int64_t{5});
    // The driving predicate is dropped -- re-checking it would be pure
    // waste, since the index probe's key match is already exact (D-047).
    FLINTDB_CHECK(plan.residual_predicates.empty());
}

FLINTDB_TEST(planner_equality_on_secondary_indexed_column_drives_an_index_scan) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"sku", ColumnType::kInteger}}, "id");
    cat.CreateIndex("widgets_sku_idx", "widgets", "sku");

    TableAccessPlan plan =
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE sku = 42"));
    FLINTDB_CHECK(plan.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(plan.index_name, std::string("widgets_sku_idx"));
    FLINTDB_CHECK_EQ(plan.index_key, int64_t{42});
    FLINTDB_CHECK(plan.residual_predicates.empty());
}

FLINTDB_TEST(planner_equality_on_a_non_indexed_column_is_a_sequential_scan) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    TableAccessPlan plan =
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE name = 'bolt'"));
    FLINTDB_CHECK(plan.method == ScanMethod::kSeqScan);
    FLINTDB_CHECK_EQ(plan.residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(plan.residual_predicates[0].column.column_name, std::string("name"));
}

FLINTDB_TEST(planner_non_equality_comparators_on_an_indexed_column_never_drive_an_index_scan) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");

    for (const std::string op : {"!=", "<", "<=", ">", ">="}) {
        TableAccessPlan plan = PlanTableAccess(
            cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE id " + op + " 5"));
        FLINTDB_CHECK(plan.method == ScanMethod::kSeqScan);
        FLINTDB_CHECK_EQ(plan.residual_predicates.size(), 1u);
    }
}

FLINTDB_TEST(planner_only_the_driving_predicate_is_removed_others_remain_residual) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"qty", ColumnType::kInteger}}, "id");

    TableAccessPlan plan = PlanTableAccess(
        cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE id = 5 AND qty > 0 AND qty < 100"));
    FLINTDB_CHECK(plan.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(plan.index_key, int64_t{5});
    FLINTDB_CHECK_EQ(plan.residual_predicates.size(), 2u);
    FLINTDB_CHECK(plan.residual_predicates[0].column.column_name == "qty");
    FLINTDB_CHECK(plan.residual_predicates[1].column.column_name == "qty");
}

FLINTDB_TEST(planner_tie_break_among_several_eligible_predicates_is_where_clause_order_D047) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"sku", ColumnType::kInteger}}, "id");
    cat.CreateIndex("widgets_sku_idx", "widgets", "sku");

    // "id" first -> "id" drives, "sku" is residual.
    TableAccessPlan plan_id_first =
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE id = 1 AND sku = 2"));
    FLINTDB_CHECK(plan_id_first.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(plan_id_first.index_name, cat.PrimaryKeyIndex("widgets")->name);
    FLINTDB_CHECK_EQ(plan_id_first.residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(plan_id_first.residual_predicates[0].column.column_name, std::string("sku"));

    // Same predicates, reversed order -> "sku" drives instead, proving
    // the tie-break really is source order, not e.g. index creation
    // order or column declaration order (both of which would still favor
    // "id" here).
    TableAccessPlan plan_sku_first =
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE sku = 2 AND id = 1"));
    FLINTDB_CHECK(plan_sku_first.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(plan_sku_first.index_name, std::string("widgets_sku_idx"));
    FLINTDB_CHECK_EQ(plan_sku_first.residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(plan_sku_first.residual_predicates[0].column.column_name, std::string("id"));
}

FLINTDB_TEST(planner_table_access_on_unknown_table_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    bool threw = false;
    try {
        PlanTableAccess(cat, "no_such_table", {});
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_predicate_on_unknown_column_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
    bool threw = false;
    try {
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE ghost = 1"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_predicate_literal_type_mismatch_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    bool threw_int_col_vs_text_literal = false;
    try {
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE id = 'oops'"));
    } catch (const SqlSemanticError&) {
        threw_int_col_vs_text_literal = true;
    }
    FLINTDB_CHECK(threw_int_col_vs_text_literal);

    bool threw_text_col_vs_int_literal = false;
    try {
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE name = 5"));
    } catch (const SqlSemanticError&) {
        threw_text_col_vs_int_literal = true;
    }
    FLINTDB_CHECK(threw_text_col_vs_int_literal);
}

FLINTDB_TEST(planner_predicate_qualified_with_a_table_not_in_scope_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
    bool threw = false;
    try {
        // Only "widgets" is in scope for a join-free PlanTableAccess call
        // -- a predicate qualified with any other table name is a
        // semantic error, not merely ignored.
        PlanTableAccess(cat, "widgets", WherePredicatesOf("SELECT * FROM widgets WHERE gadgets.id = 1"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

// ---------------------------------------------------------------------
// PlanSelect: join-free SELECT (delegates to PlanTableAccess, plus
// select_list resolution).
// ---------------------------------------------------------------------

FLINTDB_TEST(planner_select_star_leaves_select_list_empty_and_unused) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");

    SelectPlan plan = PlanSelect(cat, ParseSelect("SELECT * FROM widgets WHERE id = 1"));
    FLINTDB_CHECK(plan.select_star);
    FLINTDB_CHECK(plan.select_list.empty());
    FLINTDB_CHECK(std::holds_alternative<TableAccessPlan>(plan.access));
    FLINTDB_CHECK(std::get<TableAccessPlan>(plan.access).method == ScanMethod::kIndexScan);
}

FLINTDB_TEST(planner_select_explicit_list_is_resolved_with_table_name_always_filled_in) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    // The AST's own column_ref is unqualified ("id", not "widgets.id"),
    // but the resolved plan must fill in table_name regardless -- see
    // docs/DECISIONS.md D-047.
    SelectPlan plan = PlanSelect(cat, ParseSelect("SELECT id, name FROM widgets"));
    FLINTDB_CHECK(!plan.select_star);
    FLINTDB_CHECK_EQ(plan.select_list.size(), 2u);
    FLINTDB_CHECK(plan.select_list[0].table_name.has_value());
    FLINTDB_CHECK_EQ(*plan.select_list[0].table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(plan.select_list[0].column_name, std::string("id"));
    FLINTDB_CHECK_EQ(*plan.select_list[1].table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(plan.select_list[1].column_name, std::string("name"));
}

FLINTDB_TEST(planner_select_list_with_unknown_column_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
    bool threw = false;
    try {
        PlanSelect(cat, ParseSelect("SELECT ghost FROM widgets"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_select_from_unknown_table_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    bool threw = false;
    try {
        PlanSelect(cat, ParseSelect("SELECT * FROM ghosts"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

// ---------------------------------------------------------------------
// PlanSelect: JOIN (equi-join, outer=FROM/inner=JOIN, D-047).
// ---------------------------------------------------------------------

namespace {

// customers(id INTEGER PRIMARY KEY, name TEXT, status TEXT) and
// orders(id INTEGER PRIMARY KEY, customer_id INTEGER, qty INTEGER),
// shared by every JOIN test below. `with_join_column_index` additionally
// indexes orders.customer_id, the column every test below joins on --
// tests exercise both with and without it, to check D-047's inner-side
// index-vs-scan rule.
void BuildOrdersAndCustomers(Catalog& cat, bool with_join_column_index) {
    cat.CreateTable("customers", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}, {"status", ColumnType::kText}},
                     "id");
    cat.CreateTable("orders", {{"id", ColumnType::kInteger}, {"customer_id", ColumnType::kInteger}, {"qty", ColumnType::kInteger}},
                     "id");
    if (with_join_column_index) {
        cat.CreateIndex("orders_customer_id_idx", "orders", "customer_id");
    }
}

}  // namespace

FLINTDB_TEST(planner_join_inner_uses_index_scan_when_join_column_is_indexed) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/true);

    SelectPlan plan = PlanSelect(
        cat, ParseSelect("SELECT * FROM customers JOIN orders ON customers.id = orders.customer_id"));
    FLINTDB_CHECK(std::holds_alternative<JoinPlan>(plan.access));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK_EQ(jp.outer.table_name, std::string("customers"));
    FLINTDB_CHECK(jp.outer.method == ScanMethod::kSeqScan);  // no WHERE at all here
    FLINTDB_CHECK_EQ(jp.inner_table_name, std::string("orders"));
    FLINTDB_CHECK(jp.inner_method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(jp.inner_index_name, std::string("orders_customer_id_idx"));
    FLINTDB_CHECK_EQ(jp.outer_join_column, std::string("id"));
    FLINTDB_CHECK_EQ(jp.inner_join_column, std::string("customer_id"));
}

FLINTDB_TEST(planner_join_inner_falls_back_to_sequential_scan_when_join_column_is_not_indexed) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);

    SelectPlan plan = PlanSelect(
        cat, ParseSelect("SELECT * FROM customers JOIN orders ON customers.id = orders.customer_id"));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK(jp.inner_method == ScanMethod::kSeqScan);
    FLINTDB_CHECK(jp.inner_index_name.empty());
}

FLINTDB_TEST(planner_join_on_clause_order_does_not_matter_for_which_side_is_outer_vs_inner) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/true);

    // "orders.customer_id = customers.id" -- reversed from the test
    // above -- must still resolve orders as inner (the JOIN table) and
    // customers as outer (the FROM table), regardless of which side of
    // "=" each column appears on.
    SelectPlan plan = PlanSelect(
        cat, ParseSelect("SELECT * FROM customers JOIN orders ON orders.customer_id = customers.id"));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK_EQ(jp.outer.table_name, std::string("customers"));
    FLINTDB_CHECK_EQ(jp.inner_table_name, std::string("orders"));
    FLINTDB_CHECK_EQ(jp.outer_join_column, std::string("id"));
    FLINTDB_CHECK_EQ(jp.inner_join_column, std::string("customer_id"));
    FLINTDB_CHECK(jp.inner_method == ScanMethod::kIndexScan);
}

FLINTDB_TEST(planner_join_outer_where_predicate_still_gets_its_own_index_scan) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/true);

    SelectPlan plan = PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN orders ON "
                                                   "customers.id = orders.customer_id WHERE customers.id = 7"));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK(jp.outer.method == ScanMethod::kIndexScan);
    FLINTDB_CHECK_EQ(jp.outer.index_name, cat.PrimaryKeyIndex("customers")->name);
    FLINTDB_CHECK_EQ(jp.outer.index_key, int64_t{7});
}

FLINTDB_TEST(planner_join_inner_where_predicate_is_always_residual_even_when_its_own_column_is_indexed_D047) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);
    // "qty" has its own index, but it's not the join column -- D-047
    // says this must never change inner_method, which stays kSeqScan
    // (there's no index on customer_id in this test) regardless.
    cat.CreateIndex("orders_qty_idx", "orders", "qty");

    SelectPlan plan = PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN orders ON "
                                                   "customers.id = orders.customer_id WHERE orders.qty = 3"));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK(jp.inner_method == ScanMethod::kSeqScan);
    FLINTDB_CHECK_EQ(jp.inner_residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(jp.inner_residual_predicates[0].column.column_name, std::string("qty"));
}

FLINTDB_TEST(planner_join_where_predicates_are_split_by_table) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);

    SelectPlan plan = PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN orders ON "
                                                   "customers.id = orders.customer_id "
                                                   "WHERE customers.status = 'active' AND orders.qty > 0"));
    const JoinPlan& jp = std::get<JoinPlan>(plan.access);
    FLINTDB_CHECK_EQ(jp.outer.residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(jp.outer.residual_predicates[0].column.column_name, std::string("status"));
    FLINTDB_CHECK_EQ(jp.inner_residual_predicates.size(), 1u);
    FLINTDB_CHECK_EQ(jp.inner_residual_predicates[0].column.column_name, std::string("qty"));
}

FLINTDB_TEST(planner_join_select_list_resolves_qualified_and_unqualified_columns_to_the_right_table) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/true);

    // "name" is unambiguous (only customers has it); "qty" is unambiguous
    // (only orders has it); "customers.id" is explicitly qualified.
    SelectPlan plan = PlanSelect(
        cat, ParseSelect("SELECT customers.id, name, qty FROM customers JOIN orders ON "
                          "customers.id = orders.customer_id"));
    FLINTDB_CHECK_EQ(plan.select_list.size(), 3u);
    FLINTDB_CHECK_EQ(*plan.select_list[0].table_name, std::string("customers"));
    FLINTDB_CHECK_EQ(plan.select_list[0].column_name, std::string("id"));
    FLINTDB_CHECK_EQ(*plan.select_list[1].table_name, std::string("customers"));
    FLINTDB_CHECK_EQ(plan.select_list[1].column_name, std::string("name"));
    FLINTDB_CHECK_EQ(*plan.select_list[2].table_name, std::string("orders"));
    FLINTDB_CHECK_EQ(plan.select_list[2].column_name, std::string("qty"));
}

FLINTDB_TEST(planner_join_ambiguous_unqualified_column_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    // Both tables declare "id" -- an unqualified reference to it is
    // ambiguous once both are in scope via JOIN.
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);
    bool threw = false;
    try {
        PlanSelect(cat, ParseSelect("SELECT id FROM customers JOIN orders ON "
                                    "customers.id = orders.customer_id"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_join_unknown_join_table_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("customers", {{"id", ColumnType::kInteger}}, "id");
    bool threw = false;
    try {
        PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN ghosts ON customers.id = ghosts.id"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_self_join_throws_semantic_error_D047) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
    bool threw = false;
    try {
        PlanSelect(cat, ParseSelect("SELECT * FROM widgets JOIN widgets ON widgets.id = widgets.id"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_join_on_clause_referencing_only_one_table_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);
    bool threw = false;
    try {
        // Both sides of ON qualify to "orders" -- not a cross-table
        // equi-join condition at all.
        PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN orders ON orders.id = orders.qty"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(planner_join_on_clause_with_mismatched_column_types_throws_semantic_error) {
    TempDir dir;
    Catalog cat(dir.path());
    BuildOrdersAndCustomers(cat, /*with_join_column_index=*/false);
    bool threw = false;
    try {
        // customers.name is TEXT, orders.id is INTEGER -- never a
        // meaningful equi-join condition.
        PlanSelect(cat, ParseSelect("SELECT * FROM customers JOIN orders ON customers.name = orders.id"));
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}
