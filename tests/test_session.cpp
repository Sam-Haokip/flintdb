// Tests for session.h's Execute (task #44): transaction lifecycle
// (BEGIN/COMMIT/ROLLBACK) and the implicit "autocommit" policy for every
// other statement -- see docs/DECISIONS.md D-049 for the design.
#include "../src/db/database.h"
#include "../src/sql/parser.h"
#include "../src/sql/session.h"
#include "../src/sql/sql_error.h"
#include "test_framework.h"
#include "test_utils.h"

#include <cstdint>
#include <stdexcept>

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {
ExecuteResult Run(Database& db, const std::string& sql) { return Execute(db, Parse(sql)); }
}  // namespace

// ---------------------------------------------------------------------
// Autocommit: a bare statement with no BEGIN gets its own implicit
// transaction.
// ---------------------------------------------------------------------

FLINTDB_TEST(session_bare_insert_autocommits_immediately) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");

    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());
    Run(db, "INSERT INTO widgets VALUES (1)");
    // The implicit transaction this INSERT ran in must have already been
    // committed and cleaned up by the time Execute returns -- not left
    // open for the caller to somehow close.
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());

    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 1u);
}

FLINTDB_TEST(session_bare_select_with_no_transaction_still_returns_correct_rows) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    Run(db, "INSERT INTO widgets VALUES (1)");
    Run(db, "INSERT INTO widgets VALUES (2)");

    ExecuteResult result = Run(db, "SELECT * FROM widgets WHERE id = 2");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());
}

FLINTDB_TEST(session_autocommit_aborts_cleanly_on_a_semantic_error_leaving_no_partial_effect) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    Run(db, "INSERT INTO widgets VALUES (1)");

    bool threw = false;
    try {
        Run(db, "INSERT INTO widgets VALUES (1)");  // duplicate PRIMARY KEY
    } catch (const SqlSemanticError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    // The implicit transaction that failed must have been cleanly
    // aborted -- not left dangling for a later Execute to trip over.
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 1u);

    // A subsequent statement must still work fine.
    Run(db, "INSERT INTO widgets VALUES (2)");
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 2u);
}

// ---------------------------------------------------------------------
// Explicit BEGIN/COMMIT/ROLLBACK.
// ---------------------------------------------------------------------

FLINTDB_TEST(session_begin_commit_makes_every_statement_in_between_durable_together) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");

    Run(db, "BEGIN");
    FLINTDB_CHECK(db.GetTransactionManager().HasActiveTransaction());
    Run(db, "INSERT INTO widgets VALUES (1)");
    Run(db, "INSERT INTO widgets VALUES (2)");
    // Still inside the same explicit transaction -- neither INSERT above
    // should have committed (or started a second, nested) one of its own.
    FLINTDB_CHECK(db.GetTransactionManager().HasActiveTransaction());
    Run(db, "COMMIT");
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());

    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets").rows.size(), 2u);
}

FLINTDB_TEST(session_begin_rollback_undoes_every_statement_in_between) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    Run(db, "INSERT INTO widgets VALUES (1)");  // committed beforehand, survives the rollback below

    Run(db, "BEGIN");
    Run(db, "INSERT INTO widgets VALUES (2)");
    Run(db, "INSERT INTO widgets VALUES (3)");
    Run(db, "ROLLBACK");
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());

    ExecuteResult result = Run(db, "SELECT * FROM widgets");
    FLINTDB_CHECK_EQ(result.rows.size(), 1u);
    FLINTDB_CHECK_EQ(result.rows[0][0].int_value, int64_t{1});
}

FLINTDB_TEST(session_commit_with_no_active_transaction_throws_logic_error) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        Run(db, "COMMIT");
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(session_rollback_with_no_active_transaction_throws_logic_error) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        Run(db, "ROLLBACK");
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(session_begin_while_already_in_a_transaction_throws_logic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "BEGIN");
    bool threw = false;
    try {
        Run(db, "BEGIN");
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    // Clean up -- the failed second BEGIN must not have disturbed the
    // first, still-active transaction.
    FLINTDB_CHECK(db.GetTransactionManager().HasActiveTransaction());
    Run(db, "ROLLBACK");
}

// ---------------------------------------------------------------------
// DDL: never wrapped in an implicit transaction, and still refuses to
// run inside an explicit one.
// ---------------------------------------------------------------------

FLINTDB_TEST(session_create_table_runs_directly_with_no_transaction_needed) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY)");
    FLINTDB_CHECK(db.GetCatalog().FindTable("widgets") != nullptr);
    FLINTDB_CHECK(!db.GetTransactionManager().HasActiveTransaction());
}

FLINTDB_TEST(session_create_table_while_an_explicit_transaction_is_active_throws_logic_error) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "BEGIN");
    bool threw = false;
    try {
        Run(db, "CREATE TABLE widgets (id INTEGER)");
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK(db.GetCatalog().FindTable("widgets") == nullptr);
    Run(db, "ROLLBACK");
}

FLINTDB_TEST(session_create_index_runs_directly_with_no_transaction_needed) {
    TempDir dir;
    Database db(dir.path());
    Run(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, qty INTEGER)");
    Run(db, "INSERT INTO widgets VALUES (1, 5)");
    Run(db, "CREATE INDEX widgets_qty_idx ON widgets(qty)");
    FLINTDB_CHECK(db.GetCatalog().FindIndex("widgets_qty_idx") != nullptr);
    FLINTDB_CHECK_EQ(Run(db, "SELECT * FROM widgets WHERE qty = 5").rows.size(), 1u);
}
