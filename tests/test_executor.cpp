// Tests for the executor (src/sql/executor.h/.cpp, task #43): end-to-end
// CREATE TABLE/CREATE INDEX/INSERT/SELECT/UPDATE/DELETE against a real
// Database, including PRIMARY KEY uniqueness enforcement, full index
// maintenance across UPDATE's delete-then-reinsert, CREATE INDEX
// backfill (and its durability across a reopen), and JOIN execution --
// see docs/DECISIONS.md D-048 for the design these tests check.
#include "../src/db/database.h"
#include "../src/sql/executor.h"
#include "../src/sql/parser.h"
#include "../src/sql/sql_error.h"
#include "test_framework.h"
#include "test_utils.h"

#include <algorithm>

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {

// Runs one statement with no ambient transaction -- for DDL (which must
// run without one, docs/DECISIONS.md D-048) and for SELECT (which works
// fine either way; kept transaction-free here for simplicity).
ExecuteResult Run(Database& db, const std::string& sql) { return ExecuteInCurrentTransaction(db, Parse(sql)); }

// Runs one statement wrapped in its own explicit BEGIN/COMMIT (or ABORT
// on any exception) -- the pattern executor.h's own class comment
// documents for a caller that wants one DML statement to run
// transactionally, since Execute itself doesn't do this (task #44's job).
ExecuteResult RunInTxn(Database& db, const std::string& sql) {
    Transaction* txn = db.GetTransactionManager().Begin();
    try {
        ExecuteResult result = ExecuteInCurrentTransaction(db, Parse(sql));
        db.GetTransactionManager().Commit(txn);
        return result;
    } catch (...) {
        db.GetTransactionManager().Abort(txn);
        throw;
    }
}

bool HasRowWithInt(const ExecuteResult& result, size_t col, int64_t value) {
    for (const auto& row : result.rows) {
        if (row[col].int_value == value) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------
// CREATE TABLE / INSERT / SELECT (no WHERE, no JOIN).
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_create_table_insert_and_select_star_round_trips) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'bolt')");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 'nut')");

    ExecuteResult result = Run(db, "SELECT * FROM widgets");
    FLINTDB_CHECK_EQ(result.column_names.size(), 2u);
    FLINTDB_CHECK_EQ(result.column_names[0], std::string("id"));
    FLINTDB_CHECK_EQ(result.column_names[1], std::string("name"));
    FLINTDB_CHECK_EQ(result.rows.size(), 2u);
    FLINTDB_CHECK(HasRowWithInt(result, 0, 1));
    FLINTDB_CHECK(HasRowWithInt(result, 0, 2));
}

FLINTDB_TEST(executor_insert_returns_rows_affected_equal_to_one) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    ExecuteResult result = RunInTxn(db, "INSERT INTO widgets VALUES (1)");
    FLINTDB_CHECK_EQ(result.rows_affected, 1u);
}

FLINTDB_TEST(executor_insert_into_table_without_primary_key_has_no_uniqueness_check) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'a')");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'b')");  // duplicate "id" is fine -- no PRIMARY KEY here

    ExecuteResult result = Run(db, "SELECT * FROM widgets");
    FLINTDB_CHECK_EQ(result.rows.size(), 2u);
}

FLINTDB_TEST(executor_insert_wrong_value_count_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER, name TEXT)");
    bool threw = false;
    try {
        RunInTxn(db, "INSERT INTO widgets VALUES (1)");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 0u);
}

FLINTDB_TEST(executor_insert_value_type_mismatch_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER, name TEXT)");
    bool threw = false;
    try {
        RunInTxn(db, "INSERT INTO widgets VALUES ('oops', 'a')");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(executor_insert_into_unknown_table_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        RunInTxn(db, "INSERT INTO ghosts VALUES (1)");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

// ---------------------------------------------------------------------
// PRIMARY KEY uniqueness (task #43's other named deliverable).
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_insert_duplicate_primary_key_throws_and_leaves_no_partial_row) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'first')");

    bool threw = false;
    try {
        RunInTxn(db, "INSERT INTO widgets VALUES (1, 'second')");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    // Exactly the original row survives -- the rejected INSERT touched
    // neither the heap file nor any index.
    ExecuteResult result = Run(db, "SELECT * FROM widgets");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][1].string_value, std::string("first"));

    // The Catalog/Database themselves are unaffected -- a normal INSERT
    // still works fine afterward.
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 'third')");
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 2u);
}

// ---------------------------------------------------------------------
// SELECT with WHERE -- both the sequential-scan and index-scan access
// paths (FetchCandidateRows), and residual-predicate filtering.
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_select_where_on_indexed_column_uses_index_scan_path_and_returns_the_right_row) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'bolt')");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 'nut')");
    RunInTxn(db, "INSERT INTO widgets VALUES (3, 'washer')");

    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE id = 2");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{2});
    FLINTDB_CHECK_EQ(result.rows[0][1].string_value, std::string("nut"));
}

FLINTDB_TEST(executor_select_where_on_non_indexed_column_filters_via_sequential_scan) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 5)");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 10)");
    RunInTxn(db, "INSERT INTO widgets VALUES (3, 10)");

    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE qty = 10");
    FLINTDB_CHECK_EQ(result.rows.size(), 2u);
    FLINTDB_CHECK(HasRowWithInt(result, 0, 2));
    FLINTDB_CHECK(HasRowWithInt(result, 0, 3));
}

FLINTDB_TEST(executor_select_where_with_and_conjunction_applies_every_predicate) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 10)");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 10)");
    RunInTxn(db, "INSERT INTO widgets VALUES (3, 20)");

    // "id" drives the index scan; "qty > 5" is a residual filter still
    // applied on top of it.
    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE id = 1 AND qty > 5");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{1});

    ExecuteResult none = Run(db, "SELECT * FROM widgets WHERE id = 1 AND qty > 50");
    FLINTDB_CHECK_EQ(none.rows.size(), 0u);
}

FLINTDB_TEST(executor_select_explicit_column_list_projects_only_those_columns_in_order) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT, qty INTEGER)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'bolt', 5)");

    ExecuteResult result = Run(db, "SELECT qty, id FROM widgets WHERE id = 1");
    FLINTDB_CHECK_EQ(result.column_names.size(), 2u);
    FLINTDB_CHECK_EQ(result.column_names[0], std::string("qty"));
    FLINTDB_CHECK_EQ(result.column_names[1], std::string("id"));
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{5});
    FLINTDB_CHECK_EQ(result.rows[0][1].int_value, int64_t{1});
}

FLINTDB_TEST(executor_select_from_unknown_table_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        Run(db, "SELECT * FROM ghosts");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

// ---------------------------------------------------------------------
// DELETE.
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_delete_removes_matching_rows_and_their_index_entries) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
    Run(db, "CREATE INDEX widgets_qty_idx ON widgets(qty)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 10)");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 20)");

    ExecuteResult deleted = RunInTxn(db, "DELETE FROM widgets WHERE id = 1");
    FLINTDB_CHECK_EQ(deleted.rows_affected, 1u);

    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 1u);
    // The secondary index entry for the deleted row must be gone too --
    // not just the heap row -- or this WHERE would still find it.
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets WHERE qty = 10").rows.size(), 0u);
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets WHERE qty = 20").rows.size(), 1u);
}

FLINTDB_TEST(executor_delete_from_unknown_table_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        RunInTxn(db, "DELETE FROM ghosts");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(executor_delete_without_where_removes_every_row) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1)");
    RunInTxn(db, "INSERT INTO widgets VALUES (2)");

    ExecuteResult deleted = RunInTxn(db, "DELETE FROM widgets");
    FLINTDB_CHECK_EQ(deleted.rows_affected, 2u);
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 0u);
}

// ---------------------------------------------------------------------
// UPDATE -- delete-then-reinsert, full index maintenance, and PRIMARY
// KEY re-checking (docs/DECISIONS.md D-048).
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_update_changes_matching_rows_and_leaves_others_untouched) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'old')");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 'unchanged')");

    ExecuteResult updated = RunInTxn(db, "UPDATE widgets SET name = 'new' WHERE id = 1");
    FLINTDB_CHECK_EQ(updated.rows_affected, 1u);

    ExecuteResult one = Run(db, "SELECT * FROM widgets WHERE id = 1");
    FLINTDB_CHECK_EQ(one.rows[0][1].string_value, std::string("new"));
    ExecuteResult two = Run(db, "SELECT * FROM widgets WHERE id = 2");
    FLINTDB_CHECK_EQ(two.rows[0][1].string_value, std::string("unchanged"));
}

FLINTDB_TEST(executor_update_moves_row_and_updates_every_index_even_untouched_ones_D048) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER, name TEXT)");
    Run(db, "CREATE INDEX widgets_qty_idx ON widgets(qty)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 10, 'a')");

    // The SET clause never mentions "id" or "qty" -- but HeapFile has no
    // in-place update (storage/heap_file.h), so this row gets a brand-new
    // RID regardless. Both the PRIMARY KEY index (on "id") and the
    // secondary index (on "qty") must have their entries updated to that
    // new RID, or a subsequent lookup through either would either find
    // nothing or (worse) point at a stale, now-deleted slot.
    RunInTxn(db, "UPDATE widgets SET name = 'b' WHERE id = 1");

    ExecuteResult via_pk = Run(db, "SELECT * FROM widgets WHERE id = 1");
    FLINTDB_CHECK_EQ(via_pk.rows.size(), 1u);
    FLINTDB_CHECK_EQ(via_pk.rows[0][2].string_value, std::string("b"));

    ExecuteResult via_secondary = Run(db, "SELECT * FROM widgets WHERE qty = 10");
    FLINTDB_CHECK_EQ(via_secondary.rows.size(), 1u);
    FLINTDB_CHECK_EQ(via_secondary.rows[0][2].string_value, std::string("b"));
}

FLINTDB_TEST(executor_update_setting_primary_key_to_a_duplicate_value_throws_and_leaves_data_unchanged) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'a')");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 'b')");

    bool threw = false;
    try {
        RunInTxn(db, "UPDATE widgets SET id = 2 WHERE id = 1");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    // Both original rows must still be exactly as they were.
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets WHERE id = 1").rows[0][1].string_value, std::string("a"));
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets WHERE id = 2").rows[0][1].string_value, std::string("b"));
}

FLINTDB_TEST(executor_update_leaving_primary_key_unchanged_does_not_spuriously_collide_with_itself) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'a')");

    // "id" isn't in the SET clause, but the delete-then-reinsert still
    // removes and re-adds its PRIMARY KEY index entry -- this must not
    // be mistaken for a collision with the row's own (just-vacated) key.
    RunInTxn(db, "UPDATE widgets SET name = 'z' WHERE id = 1");
    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE id = 1");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][1].string_value, std::string("z"));
}

FLINTDB_TEST(executor_update_unknown_target_column_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1)");
    bool threw = false;
    try {
        RunInTxn(db, "UPDATE widgets SET ghost = 1 WHERE id = 1");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(executor_update_assignment_type_mismatch_throws_semantic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 'a')");
    bool threw = false;
    try {
        RunInTxn(db, "UPDATE widgets SET name = 5 WHERE id = 1");
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

// ---------------------------------------------------------------------
// CREATE INDEX backfill (docs/DECISIONS.md D-048).
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_create_index_on_a_nonempty_table_backfills_existing_rows) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
    RunInTxn(db, "INSERT INTO widgets VALUES (1, 10)");
    RunInTxn(db, "INSERT INTO widgets VALUES (2, 20)");
    RunInTxn(db, "INSERT INTO widgets VALUES (3, 20)");

    // The index is created *after* the rows already exist -- without a
    // backfill, this index would be empty and every WHERE qty = ... below
    // would wrongly return nothing.
    Run(db, "CREATE INDEX widgets_qty_idx ON widgets(qty)");

    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE qty = 20");
    FLINTDB_CHECK_EQ(result.rows.size(), 2u);
    FLINTDB_CHECK(HasRowWithInt(result, 0, 2));
    FLINTDB_CHECK(HasRowWithInt(result, 0, 3));
}

FLINTDB_TEST(executor_create_index_backfill_survives_a_database_reopen_D048) {
    TempDir dir;
    {
        Database db(dir.path());
        Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
        RunInTxn(db, "INSERT INTO widgets VALUES (1, 10)");
        RunInTxn(db, "INSERT INTO widgets VALUES (2, 20)");
        Run(db, "CREATE INDEX widgets_qty_idx ON widgets(qty)");
    }  // Database destructed -- only what actually reached disk survives.

    Database reopened(dir.path());
    ExecuteResult result = Run(reopened, "SELECT * FROM widgets WHERE qty = 20");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{2});
}

// ---------------------------------------------------------------------
// DDL / transaction-control guardrails.
// ---------------------------------------------------------------------

FLINTDB_TEST(executor_ddl_while_a_transaction_is_active_throws_logic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");

    Transaction* txn = db.GetTransactionManager().Begin();
    bool threw = false;
    try {
        ExecuteInCurrentTransaction(db, Parse("CREATE TABLE gadgets (id INTEGER)"));
    } catch (const std::logic_error&) {
        threw = true;
    }
    db.GetTransactionManager().Abort(txn);
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK(db.GetCatalog().FindTable("gadgets") == nullptr);
}

FLINTDB_TEST(executor_in_current_transaction_has_no_meaning_for_begin_commit_rollback) {
    // ExecuteInCurrentTransaction is the raw data-manipulation mechanism
    // (docs/DECISIONS.md D-048) -- BEGIN/COMMIT/ROLLBACK have no
    // data-manipulation meaning of their own and are handled instead by
    // session.h's Execute (D-049, tests/test_session.cpp), which never
    // forwards one of these three down to this function.
    TempDir dir;
    Database db(dir.path());
    for (const std::string sql : {"BEGIN", "COMMIT", "ROLLBACK"}) {
        bool threw = false;
        try {
            ExecuteInCurrentTransaction(db, Parse(sql));
        } catch (const std::logic_error&) {
            threw = true;
        }
        FLINTDB_CHECK(threw);
    }
}

// ---------------------------------------------------------------------
// SELECT ... JOIN ... (both the index-nested-loop and naive nested-loop
// execution paths, docs/DECISIONS.md D-048).
// ---------------------------------------------------------------------

namespace {

void SeedCustomersAndOrders(Database& db, bool with_join_column_index) {
    Run(db, "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)");
    Run(db, "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, qty INTEGER)");
    if (with_join_column_index) {
        Run(db, "CREATE INDEX orders_customer_id_idx ON orders(customer_id)");
    }
    RunInTxn(db, "INSERT INTO customers VALUES (1, 'alice')");
    RunInTxn(db, "INSERT INTO customers VALUES (2, 'bob')");
    RunInTxn(db, "INSERT INTO orders VALUES (100, 1, 5)");
    RunInTxn(db, "INSERT INTO orders VALUES (101, 1, 7)");
    RunInTxn(db, "INSERT INTO orders VALUES (102, 2, 3)");
}

}  // namespace

FLINTDB_TEST(executor_join_select_star_concatenates_both_tables_columns) {
    TempDir dir;
    Database db(dir.path());
    SeedCustomersAndOrders(db, /*with_join_column_index=*/true);

    ExecuteResult result =
        Run(db, "SELECT * FROM customers JOIN orders ON customers.id = orders.customer_id WHERE customers.id = 1");
    FLINTDB_CHECK_EQ(result.column_names.size(), 5u);  // customers(id,name) + orders(id,customer_id,qty)
    FLINTDB_CHECK_EQ(result.rows.size(), 2u);  // alice has two orders
    for (const auto& row : result.rows) {
        FLINTDB_CHECK_EQ(row[0].int_value, int64_t{1});           // customers.id
        FLINTDB_CHECK_EQ(row[1].string_value, std::string("alice"));  // customers.name
        FLINTDB_CHECK_EQ(row[3].int_value, int64_t{1});           // orders.customer_id
    }
}

FLINTDB_TEST(executor_join_with_indexed_join_column_matches_naive_join_without_one) {
    TempDir dir_indexed;
    Database db_indexed(dir_indexed.path());
    SeedCustomersAndOrders(db_indexed, /*with_join_column_index=*/true);

    TempDir dir_naive;
    Database db_naive(dir_naive.path());
    SeedCustomersAndOrders(db_naive, /*with_join_column_index=*/false);

    std::string sql = "SELECT customers.name, orders.qty FROM customers JOIN orders ON "
                       "customers.id = orders.customer_id";
    ExecuteResult indexed = Run(db_indexed, sql);
    ExecuteResult naive = Run(db_naive, sql);

    FLINTDB_CHECK_EQ(indexed.rows.size(), 3u);
    FLINTDB_CHECK_EQ(naive.rows.size(), 3u);

    auto sort_key = [](const std::vector<LiteralValue>& row) {
        return row[0].string_value + "/" + std::to_string(row[1].int_value);
    };
    std::vector<std::string> indexed_keys, naive_keys;
    for (auto& row : indexed.rows) indexed_keys.push_back(sort_key(row));
    for (auto& row : naive.rows) naive_keys.push_back(sort_key(row));
    std::sort(indexed_keys.begin(), indexed_keys.end());
    std::sort(naive_keys.begin(), naive_keys.end());
    FLINTDB_CHECK(indexed_keys == naive_keys);
}

FLINTDB_TEST(executor_join_applies_outer_and_inner_where_predicates) {
    TempDir dir;
    Database db(dir.path());
    SeedCustomersAndOrders(db, /*with_join_column_index=*/false);

    // Outer predicate (customers.name) restricts to bob; inner predicate
    // (orders.qty) further restricts within bob's own orders.
    ExecuteResult result = Run(db, "SELECT orders.id FROM customers JOIN orders ON "
                                   "customers.id = orders.customer_id "
                                   "WHERE customers.name = 'bob' AND orders.qty = 3");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{102});

    ExecuteResult empty = Run(db, "SELECT orders.id FROM customers JOIN orders ON "
                                  "customers.id = orders.customer_id "
                                  "WHERE customers.name = 'bob' AND orders.qty = 999");
    FLINTDB_CHECK_EQ(empty.rows.size(), 0u);
}

FLINTDB_TEST(executor_join_on_text_columns_works_via_naive_nested_loop) {
    TempDir dir;
    Database db(dir.path());
    // TEXT columns can never be indexed (docs/DECISIONS.md D-046), so
    // this join always executes via the sequential-scan/explicit-equality
    // path (LiteralsEqual), never index-nested-loop.
    Run(db, "CREATE TABLE a (id INTEGER PRIMARY KEY, tag TEXT)");
    Run(db, "CREATE TABLE b (id INTEGER PRIMARY KEY, tag TEXT)");
    RunInTxn(db, "INSERT INTO a VALUES (1, 'red')");
    RunInTxn(db, "INSERT INTO a VALUES (2, 'blue')");
    RunInTxn(db, "INSERT INTO b VALUES (10, 'red')");
    RunInTxn(db, "INSERT INTO b VALUES (11, 'green')");

    ExecuteResult result = Run(db, "SELECT a.id, b.id FROM a JOIN b ON a.tag = b.tag");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{1});
    FLINTDB_CHECK_EQ(result.rows[0][1].int_value, int64_t{10});
}

FLINTDB_TEST(executor_join_select_explicit_list_pulls_columns_from_the_right_table) {
    TempDir dir;
    Database db(dir.path());
    SeedCustomersAndOrders(db, /*with_join_column_index=*/true);

    ExecuteResult result = Run(db, "SELECT name, qty FROM customers JOIN orders ON "
                                   "customers.id = orders.customer_id WHERE orders.id = 100");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].string_value, std::string("alice"));
    FLINTDB_CHECK_EQ(result.rows[0][1].int_value, int64_t{5});
}
