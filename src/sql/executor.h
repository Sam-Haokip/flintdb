#pragma once
#include "../db/database.h"
#include "ast.h"

#include <cstddef>
#include <string>
#include <vector>

namespace flintdb {

// The result of executing one statement.
struct ExecuteResult {
    // SELECT only: one entry per output row, each a vector<LiteralValue>
    // in column_names' order. Empty (and unused) for every other
    // statement type.
    std::vector<std::vector<LiteralValue>> rows;

    // Parallel to `rows`' own column order -- the (bare, unqualified) name
    // of each output column, so a caller can present results without
    // separately tracking the schema. Two columns from a JOIN's two
    // tables that happen to share a name appear twice, unchanged -- the
    // same thing a real SQL engine's own "SELECT *" output does.
    std::vector<std::string> column_names;

    // INSERT/UPDATE/DELETE: how many rows were affected (always 1 for
    // INSERT; 0 or more for UPDATE/DELETE). 0 for everything else,
    // including SELECT (whose row count is rows.size() instead) and DDL.
    size_t rows_affected = 0;
};

// Executes one parsed Statement (ast.h) against `db` -- the mechanism
// tying together the parser, the planner (planner.h), row encoding
// (row_codec.h), and the storage layer (Database). See
// docs/DECISIONS.md D-048 for the full design.
//
// This is deliberately the *mechanism*, not the *policy*: it performs
// CREATE TABLE/CREATE INDEX/INSERT/SELECT/UPDATE/DELETE, but never
// starts, commits, or aborts a transaction itself, and doesn't know or
// care whether one is already open -- it just runs through HeapFile/
// BPlusTree exactly as any pre-Phase-5 caller would, acquiring locks
// through whatever transaction (if any) is currently active on the
// calling thread (a no-op if none is). BEGIN/COMMIT/ROLLBACK statements
// are out of scope here entirely (std::logic_error) -- see session.h's
// `Execute`, the host-facing entry point that actually owns transaction
// *lifecycle* (starting/committing/aborting) and this engine's implicit
// "autocommit" policy (docs/DECISIONS.md D-049), and which calls this
// function once it has decided what transaction context a statement
// should run under. A caller that wants precise, manual control over
// where one statement's transaction boundary falls -- as opposed to
// D-049's default policy -- can still call this function directly and
// wrap it in its own explicit TransactionManager::Begin()/Commit()/
// Abort(), exactly like tests/test_database.cpp and this file's own
// tests do.
//
// CREATE TABLE/CREATE INDEX run directly with no access-path planning
// (docs/SPEC.md section 4: there's no data to find for either), and
// refuse to run (std::logic_error) while any transaction is active
// anywhere (docs/SPEC.md section 4: no concurrent DDL). INSERT/SELECT/
// UPDATE/DELETE go through PlanTableAccess/PlanSelect first.
//
// Throws SqlSemanticError (sql_error.h) for: everything the planner
// throws, plus an INSERT with the wrong number of values, an INSERT/
// UPDATE value whose kind doesn't match its column's declared type, and a
// duplicate PRIMARY KEY value (INSERT or UPDATE). Propagates whatever
// Catalog/Database/TransactionManager throw for lower-level failures
// (e.g. std::invalid_argument from a malformed CREATE TABLE/INDEX,
// TransactionAbortedException from a lock conflict under real
// concurrency).
ExecuteResult ExecuteInCurrentTransaction(Database& db, const Statement& stmt);

}  // namespace flintdb
