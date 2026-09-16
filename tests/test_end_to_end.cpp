// End-to-end tests for the full SQL pipeline (task #45): raw SQL text,
// run through Parse() and session.h's Execute() only -- no direct calls
// into HeapFile/BPlusTree/TransactionManager/Catalog anywhere in this
// file. This is deliberately different from every other Phase 5 test
// file, which each exercise one layer in isolation: test_lexer.cpp/
// test_parser.cpp (text -> tokens/AST), test_planner.cpp (AST -> access
// plan), test_executor.cpp (one statement at a time via
// ExecuteInCurrentTransaction, with an explicit RunInTxn wrapper doing the
// transaction bookkeeping the test itself would otherwise need), and
// test_session.cpp (the autocommit/transaction-lifecycle policy, exercised
// with small single- or two-table scenarios). None of them glue a
// realistic sequence of different statement types together the way a real
// host program actually would, and none of them close and reopen a
// Database built up purely from SQL text to check what a restart -- or a
// process crash mid-transaction -- actually leaves behind. That is this
// file's whole purpose, and directly satisfies the "a Database-level
// crash-recovery regression test ... planned for task #45" note in
// docs/DECISIONS.md D-041 (whose own regression test, in test_database.cpp,
// covers the same underlying property but by driving BPlusTree/
// TransactionManager directly, not through SQL text end to end).
#include "../src/db/database.h"
#include "../src/sql/parser.h"
#include "../src/sql/session.h"
#include "../src/sql/sql_error.h"
#include "../src/txn/transaction.h"
#include "test_framework.h"
#include "test_utils.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {
ExecuteResult Run(Database& db, const std::string& sql) { return Execute(db, Parse(sql)); }
}  // namespace

// ---------------------------------------------------------------------
// A realistic multi-table, multi-statement-type workflow, all through
// raw SQL text with no lower-layer API calls at all.
// ---------------------------------------------------------------------

FLINTDB_TEST(e2e_multi_table_workflow_through_raw_sql_text) {
    TempDir dir;
    Database db(dir.path());

    Run(db, "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)");
    Run(db, "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, amount INTEGER)");
    Run(db, "CREATE INDEX orders_customer_id_idx ON orders(customer_id)");

    Run(db, "INSERT INTO customers VALUES (1, 'alice')");
    Run(db, "INSERT INTO customers VALUES (2, 'bob')");
    Run(db, "INSERT INTO orders VALUES (100, 1, 30)");
    Run(db, "INSERT INTO orders VALUES (101, 1, 70)");
    Run(db, "INSERT INTO orders VALUES (102, 2, 15)");

    // Index-scan SELECT: orders_customer_id_idx should drive this.
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM orders WHERE customer_id = 1").rows.size(), 2u);

    // JOIN across both tables, with a WHERE predicate on the outer side.
    ExecuteResult joined = Run(
        db, "SELECT customers.name, orders.amount FROM customers JOIN orders "
            "ON customers.id = orders.customer_id WHERE customers.id = 1");
    FLINTDB_CHECK_EQ(joined.rows.size(), 2u);
    for (const auto& row : joined.rows) {
        FLINTDB_CHECK_EQ(row[0].string_value, std::string("alice"));
    }

    // UPDATE one order, DELETE another entirely.
    Run(db, "UPDATE orders SET amount = 999 WHERE id = 100");
    Run(db, "DELETE FROM orders WHERE id = 102");

    ExecuteResult alice_orders = Run(db, "SELECT id, amount FROM orders WHERE customer_id = 1");
    FLINTDB_CHECK_EQ(alice_orders.rows.size(), 2u);
    std::set<std::pair<int64_t, int64_t>> seen;
    for (const auto& row : alice_orders.rows) seen.insert({row[0].int_value, row[1].int_value});
    FLINTDB_CHECK(seen.count({100, 999}) == 1);
    FLINTDB_CHECK(seen.count({101, 70}) == 1);

    // bob's order was deleted -- and the secondary index must have been
    // maintained along with the heap (D-048: UPDATE/DELETE keep every
    // index in sync, not just the ones a WHERE clause happened to touch).
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM orders WHERE customer_id = 2").rows.size(), 0u);
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM orders").rows.size(), 2u);
}

// ---------------------------------------------------------------------
// An explicit transaction spanning several different statement types
// (not just repeated INSERTs, the way test_session.cpp's own BEGIN/COMMIT
// test does) -- commits atomically, and survives a reopen.
// ---------------------------------------------------------------------

FLINTDB_TEST(e2e_explicit_transaction_across_statement_types_commits_atomically_and_survives_reopen) {
    TempDir dir;
    {
        Database db(dir.path());
        Run(db, "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)");
        Run(db, "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, amount INTEGER)");
        Run(db, "INSERT INTO customers VALUES (1, 'alice')");
        Run(db, "INSERT INTO orders VALUES (100, 1, 30)");

        Run(db, "BEGIN");
        Run(db, "INSERT INTO customers VALUES (2, 'bob')");
        Run(db, "INSERT INTO orders VALUES (101, 2, 15)");
        Run(db, "UPDATE orders SET amount = 999 WHERE id = 100");

        // Reads-its-own-writes: a SELECT inside this still-open
        // transaction must already see every change made so far in it,
        // even though none of it has committed yet.
        FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM customers").rows.size(), 2u);
        FLINTDB_CHECK_EQ(Run(db, "SELECT amount FROM orders WHERE id = 100").rows[0][0].int_value, int64_t{999});

        Run(db, "COMMIT");
    }  // "close" the database -- nothing beyond COMMIT's own force-flush
       // (D-020) makes this durable; there is no extra flush-on-close step.

    Database reopened(dir.path());
    FLINTDB_CHECK_EQ(Run(reopened, "SELECT * FROM customers").rows.size(), 2u);
    ExecuteResult orders = Run(reopened, "SELECT id, amount FROM orders");
    FLINTDB_CHECK_EQ(orders.rows.size(), 2u);
    std::set<std::pair<int64_t, int64_t>> seen;
    for (const auto& row : orders.rows) seen.insert({row[0].int_value, row[1].int_value});
    FLINTDB_CHECK(seen.count({100, 999}) == 1);
    FLINTDB_CHECK(seen.count({101, 15}) == 1);
}

// ---------------------------------------------------------------------
// ROLLBACK undoes every statement type together, and the undo is
// genuinely durable -- not merely an in-memory revert -- confirmed by
// reopening afterward.
// ---------------------------------------------------------------------

FLINTDB_TEST(e2e_explicit_transaction_rollback_undoes_every_statement_type_and_reopen_confirms_it) {
    TempDir dir;
    {
        Database db(dir.path());
        Run(db, "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)");
        Run(db, "INSERT INTO customers VALUES (1, 'alice')");

        Run(db, "BEGIN");
        Run(db, "INSERT INTO customers VALUES (2, 'bob')");
        Run(db, "UPDATE customers SET name = 'alicia' WHERE id = 1");
        Run(db, "DELETE FROM customers WHERE id = 1");
        // Visible within the transaction: alice deleted, bob inserted.
        FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM customers").rows.size(), 1u);

        Run(db, "ROLLBACK");
        // All three statements undone together.
        ExecuteResult after = Run(db, "SELECT * FROM customers");
        FLINTDB_CHECK_EQ(after.rows.size(), 1u);
        FLINTDB_CHECK_EQ(after.rows[0][1].string_value, std::string("alice"));
    }

    Database reopened(dir.path());
    ExecuteResult final_state = Run(reopened, "SELECT * FROM customers");
    FLINTDB_CHECK_EQ(final_state.rows.size(), 1u);
    FLINTDB_CHECK_EQ(final_state.rows[0][1].string_value, std::string("alice"));
}

// ---------------------------------------------------------------------
// Restart/crash durability, driven entirely through SQL text: a mix of
// autocommit statements, a fully committed explicit transaction (pushed
// far enough to force a PRIMARY KEY index root split), and a transaction
// that was left open -- BEGIN issued, work done, but neither COMMIT nor
// ROLLBACK ever ran -- exactly what a real process crash mid-transaction
// looks like from the SQL layer's point of view (docs/SPEC.md section 5).
// ---------------------------------------------------------------------

FLINTDB_TEST(e2e_restart_durability_keeps_committed_work_and_drops_a_transaction_abandoned_by_a_simulated_crash) {
    TempDir dir;
    constexpr int64_t kBulkCount = 320;  // comfortably past a leaf's ~291-key capacity -- forces a root split
    constexpr int64_t kBulkStart = 1000;
    {
        Database db(dir.path());
        Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, tag TEXT)");

        // Autocommit: each statement is its own implicit transaction,
        // genuinely committed the instant Execute() returns (D-049).
        Run(db, "INSERT INTO widgets VALUES (1, 'autocommit-a')");
        Run(db, "INSERT INTO widgets VALUES (2, 'autocommit-b')");

        // A single explicit transaction, committed, large enough to force
        // its PRIMARY KEY index through a real root split (docs/DECISIONS.md
        // D-041's own regression test, in test_database.cpp, manufactures
        // this same shape of staleness directly on the BPlusTree; this
        // proves a split index also reopens correctly when it was built
        // purely from SQL INSERT statements).
        Run(db, "BEGIN");
        for (int64_t k = kBulkStart; k < kBulkStart + kBulkCount; k++) {
            Run(db, "INSERT INTO widgets VALUES (" + std::to_string(k) + ", 'bulk')");
        }
        Run(db, "COMMIT");

        // A transaction that BEGINs, does real work, and then is simply
        // abandoned -- no COMMIT, no ROLLBACK. This is deliberate, not an
        // oversight: it is the only way to simulate "the process crashed
        // mid-transaction" from outside the transaction itself, since a
        // real crash never gets the chance to call either.
        Run(db, "BEGIN");
        Run(db, "INSERT INTO widgets VALUES (999999, 'never-committed')");
        FLINTDB_CHECK(db.GetTransactionManager().HasActiveTransaction());
    }  // "crash": db (and the TransactionManager/Transaction it owns)
       // destructs with that transaction still open. No Commit record was
       // ever written for it, so RunRecovery on reopen must replay none of
       // its writes. Transaction::~Transaction() (src/txn/transaction.cpp)
       // already clears the thread-local "current transaction" pointer
       // (transaction.cpp's g_current_txn) whenever the transaction being
       // destroyed is the one it points to -- exactly this case, since
       // TransactionManager owns every still-active Transaction by
       // std::unique_ptr and destroys it along with itself -- so no manual
       // cleanup is needed here for later tests in this binary to see a
       // clean thread-local; the check right below confirms that
       // empirically rather than assuming it (this project's own standing
       // rule) instead of just trusting the code-reading above.
    FLINTDB_CHECK(GetCurrentTransaction() == nullptr);

    Database reopened(dir.path());
    FLINTDB_CHECK(!reopened.GetTransactionManager().HasActiveTransaction());

    ExecuteResult all = Run(reopened, "SELECT * FROM widgets");
    FLINTDB_CHECK_EQ(all.rows.size(), static_cast<size_t>(2 + kBulkCount));  // autocommit-a/b + the committed bulk block

    // The abandoned transaction's row must be entirely absent.
    FLINTDB_CHECK_EQ(Run(reopened, "SELECT * FROM widgets WHERE id = 999999").rows.size(), 0u);

    // Every committed row -- including ones on the far side of the root
    // split -- must still be reachable through the reopened PRIMARY KEY
    // index specifically (an index-scan SELECT), not merely present in a
    // sequential Scan() of the heap: this is what actually proves the
    // reopened, split index has the right root pointer.
    FLINTDB_CHECK_EQ(Run(reopened, "SELECT * FROM widgets WHERE id = 1").rows.size(), 1u);
    FLINTDB_CHECK_EQ(Run(reopened, "SELECT * FROM widgets WHERE id = " + std::to_string(kBulkStart)).rows.size(), 1u);
    FLINTDB_CHECK_EQ(
        Run(reopened, "SELECT * FROM widgets WHERE id = " + std::to_string(kBulkStart + kBulkCount - 1)).rows.size(),
        1u);
}
