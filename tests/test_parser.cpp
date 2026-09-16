#include "../src/sql/parser.h"
#include "../src/sql/sql_error.h"
#include "test_framework.h"

using namespace flintdb;

namespace {
template <typename T>
const T& As(const Statement& s) {
    return std::get<T>(s);
}
}  // namespace

FLINTDB_TEST(parser_create_table_without_primary_key) {
    Statement stmt = Parse("CREATE TABLE widgets (id INTEGER, name TEXT)");
    const auto& s = As<CreateTableStatement>(stmt);
    FLINTDB_CHECK_EQ(s.table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(s.columns.size(), 2u);
    FLINTDB_CHECK_EQ(s.columns[0].name, std::string("id"));
    FLINTDB_CHECK(s.columns[0].type == ColumnType::kInteger);
    FLINTDB_CHECK_EQ(s.columns[1].name, std::string("name"));
    FLINTDB_CHECK(s.columns[1].type == ColumnType::kText);
    FLINTDB_CHECK(!s.primary_key_column.has_value());
}

FLINTDB_TEST(parser_create_table_with_primary_key_on_a_middle_column) {
    Statement stmt = Parse("CREATE TABLE widgets (a INTEGER, id INTEGER PRIMARY KEY, b TEXT)");
    const auto& s = As<CreateTableStatement>(stmt);
    FLINTDB_CHECK_EQ(s.columns.size(), 3u);
    FLINTDB_CHECK(s.primary_key_column.has_value());
    FLINTDB_CHECK_EQ(*s.primary_key_column, std::string("id"));
}

FLINTDB_TEST(parser_create_table_keywords_are_case_insensitive) {
    Statement stmt = Parse("create table widgets (id integer primary key)");
    const auto& s = As<CreateTableStatement>(stmt);
    FLINTDB_CHECK_EQ(s.table_name, std::string("widgets"));
    FLINTDB_CHECK(s.primary_key_column.has_value());
}

FLINTDB_TEST(parser_create_table_rejects_more_than_one_primary_key) {
    bool threw = false;
    try {
        Parse("CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_create_table_with_zero_columns_is_a_syntax_error) {
    // The grammar's column_def ("," column_def)* makes the first column
    // mandatory -- this must fail at parse time, not be silently accepted
    // and deferred to Catalog.
    bool threw = false;
    try {
        Parse("CREATE TABLE t ()");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_create_table_rejects_an_unknown_column_type) {
    bool threw = false;
    try {
        Parse("CREATE TABLE t (a REAL)");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_create_index) {
    Statement stmt = Parse("CREATE INDEX widgets_name_idx ON widgets (name)");
    const auto& s = As<CreateIndexStatement>(stmt);
    FLINTDB_CHECK_EQ(s.index_name, std::string("widgets_name_idx"));
    FLINTDB_CHECK_EQ(s.table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(s.column_name, std::string("name"));
}

FLINTDB_TEST(parser_insert_with_mixed_literal_types) {
    Statement stmt = Parse("INSERT INTO widgets VALUES (1, 'a widget', -3)");
    const auto& s = As<InsertStatement>(stmt);
    FLINTDB_CHECK_EQ(s.table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(s.values.size(), 3u);
    FLINTDB_CHECK(s.values[0].kind == LiteralValue::Kind::kInteger);
    FLINTDB_CHECK_EQ(s.values[0].int_value, 1);
    FLINTDB_CHECK(s.values[1].kind == LiteralValue::Kind::kString);
    FLINTDB_CHECK_EQ(s.values[1].string_value, std::string("a widget"));
    FLINTDB_CHECK(s.values[2].kind == LiteralValue::Kind::kInteger);
    FLINTDB_CHECK_EQ(s.values[2].int_value, -3);
}

FLINTDB_TEST(parser_select_star_with_no_where_or_join) {
    Statement stmt = Parse("SELECT * FROM widgets");
    const auto& s = As<SelectStatement>(stmt);
    FLINTDB_CHECK(s.select_star);
    FLINTDB_CHECK(s.select_list.empty());
    FLINTDB_CHECK_EQ(s.from_table, std::string("widgets"));
    FLINTDB_CHECK(!s.join.has_value());
    FLINTDB_CHECK(s.where_predicates.empty());
}

FLINTDB_TEST(parser_select_explicit_column_list_qualified_and_unqualified) {
    Statement stmt = Parse("SELECT a.id, name FROM widgets");
    const auto& s = As<SelectStatement>(stmt);
    FLINTDB_CHECK(!s.select_star);
    FLINTDB_CHECK_EQ(s.select_list.size(), 2u);
    FLINTDB_CHECK(s.select_list[0].table_name.has_value());
    FLINTDB_CHECK_EQ(*s.select_list[0].table_name, std::string("a"));
    FLINTDB_CHECK_EQ(s.select_list[0].column_name, std::string("id"));
    FLINTDB_CHECK(!s.select_list[1].table_name.has_value());
    FLINTDB_CHECK_EQ(s.select_list[1].column_name, std::string("name"));
}

FLINTDB_TEST(parser_select_with_join_and_single_where_predicate) {
    Statement stmt = Parse("SELECT * FROM a JOIN b ON a.id = b.a_id WHERE a.qty > 5");
    const auto& s = As<SelectStatement>(stmt);
    FLINTDB_CHECK_EQ(s.from_table, std::string("a"));
    FLINTDB_CHECK(s.join.has_value());
    FLINTDB_CHECK_EQ(s.join->table_name, std::string("b"));
    FLINTDB_CHECK_EQ(*s.join->left_column.table_name, std::string("a"));
    FLINTDB_CHECK_EQ(s.join->left_column.column_name, std::string("id"));
    FLINTDB_CHECK_EQ(*s.join->right_column.table_name, std::string("b"));
    FLINTDB_CHECK_EQ(s.join->right_column.column_name, std::string("a_id"));
    FLINTDB_CHECK_EQ(s.where_predicates.size(), 1u);
    FLINTDB_CHECK(s.where_predicates[0].op == Comparator::kGt);
    FLINTDB_CHECK_EQ(s.where_predicates[0].value.int_value, 5);
}

FLINTDB_TEST(parser_join_requires_equals_and_rejects_other_comparators) {
    bool threw = false;
    try {
        Parse("SELECT * FROM a JOIN b ON a.id < b.id");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_where_clause_with_and_conjunction_of_all_comparators) {
    Statement stmt = Parse(
        "SELECT * FROM t WHERE a = 1 AND b != 2 AND c < 3 AND d <= 4 AND e > 5 AND f >= 6");
    const auto& s = As<SelectStatement>(stmt);
    FLINTDB_CHECK_EQ(s.where_predicates.size(), 6u);
    Comparator expected[] = {Comparator::kEq, Comparator::kNeq, Comparator::kLt,
                             Comparator::kLe, Comparator::kGt, Comparator::kGe};
    for (size_t i = 0; i < 6; i++) FLINTDB_CHECK(s.where_predicates[i].op == expected[i]);
}

FLINTDB_TEST(parser_where_clause_rejects_or) {
    // docs/SPEC.md section 1.3: WHERE supports AND only, no OR.
    bool threw = false;
    try {
        Parse("SELECT * FROM t WHERE a = 1 OR b = 2");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_update_with_multiple_assignments_and_where) {
    Statement stmt = Parse("UPDATE widgets SET qty = 5, name = 'new' WHERE id = 1");
    const auto& s = As<UpdateStatement>(stmt);
    FLINTDB_CHECK_EQ(s.table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(s.assignments.size(), 2u);
    FLINTDB_CHECK_EQ(s.assignments[0].column_name, std::string("qty"));
    FLINTDB_CHECK_EQ(s.assignments[0].value.int_value, 5);
    FLINTDB_CHECK_EQ(s.assignments[1].column_name, std::string("name"));
    FLINTDB_CHECK_EQ(s.assignments[1].value.string_value, std::string("new"));
    FLINTDB_CHECK_EQ(s.where_predicates.size(), 1u);
}

FLINTDB_TEST(parser_update_without_where_clause_updates_everything) {
    Statement stmt = Parse("UPDATE widgets SET qty = 0");
    const auto& s = As<UpdateStatement>(stmt);
    FLINTDB_CHECK(s.where_predicates.empty());
}

FLINTDB_TEST(parser_delete_with_and_without_where) {
    // Note: the parsed Statement must be kept in a named local before
    // calling As<>() on it -- binding As<>()'s return value directly to a
    // reference from a temporary Parse(...) result (as an earlier draft
    // of this test did) is a real dangling reference, since the temporary
    // Statement is destroyed at the end of the full expression, before
    // the reference is ever used. Caught by GCC's -Wdangling-reference.
    Statement stmt_with_where = Parse("DELETE FROM widgets WHERE id = 1");
    const auto& with_where = As<DeleteStatement>(stmt_with_where);
    FLINTDB_CHECK_EQ(with_where.table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(with_where.where_predicates.size(), 1u);

    Statement stmt_without_where = Parse("DELETE FROM widgets");
    const auto& without_where = As<DeleteStatement>(stmt_without_where);
    FLINTDB_CHECK(without_where.where_predicates.empty());
}

FLINTDB_TEST(parser_begin_commit_rollback_are_bare_statements) {
    FLINTDB_CHECK(std::holds_alternative<BeginStatement>(Parse("BEGIN")));
    FLINTDB_CHECK(std::holds_alternative<CommitStatement>(Parse("COMMIT")));
    FLINTDB_CHECK(std::holds_alternative<RollbackStatement>(Parse("ROLLBACK")));
}

FLINTDB_TEST(parser_optional_trailing_semicolon_is_accepted) {
    Statement stmt = Parse("BEGIN;");
    FLINTDB_CHECK(std::holds_alternative<BeginStatement>(stmt));
}

FLINTDB_TEST(parser_trailing_garbage_after_a_statement_is_a_syntax_error) {
    bool threw = false;
    try {
        Parse("BEGIN COMMIT");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_two_statements_separated_by_a_semicolon_is_a_syntax_error) {
    // No multi-statement script mode -- one Parse() call is one statement.
    bool threw = false;
    try {
        Parse("BEGIN; COMMIT");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_unknown_statement_keyword_is_a_syntax_error) {
    bool threw = false;
    try {
        Parse("EXPLAIN SELECT * FROM t");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_empty_input_is_a_syntax_error) {
    bool threw = false;
    try {
        Parse("");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_select_missing_from_is_a_syntax_error) {
    bool threw = false;
    try {
        Parse("SELECT * widgets");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(parser_create_without_table_or_index_is_a_syntax_error) {
    bool threw = false;
    try {
        Parse("CREATE widgets (id INTEGER)");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}
