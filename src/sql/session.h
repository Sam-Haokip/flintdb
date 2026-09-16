#pragma once
#include "../db/database.h"
#include "ast.h"
#include "executor.h"

namespace flintdb {

// The top-level, host-facing entry point for running one parsed
// Statement against `db`. Unlike executor.h's ExecuteInCurrentTransaction
// (the raw data-manipulation mechanism, task #43/D-048), this function
// also owns transaction *lifecycle*:
//
//   - BEGIN actually starts a transaction (TransactionManager::Begin()).
//   - COMMIT/ROLLBACK actually end this thread's currently active one
//     (TransactionManager::Commit()/Abort()).
//   - Every other statement (CREATE TABLE/CREATE INDEX/INSERT/SELECT/
//     UPDATE/DELETE) runs via ExecuteInCurrentTransaction, under whatever
//     transaction context already applies -- see docs/DECISIONS.md D-049
//     for the full "autocommit" policy this implements: if this thread
//     already has an explicit transaction open (a prior call here saw a
//     BEGIN not yet followed by COMMIT/ROLLBACK), the statement simply
//     joins it; otherwise (the common case for a single ad hoc
//     statement) it runs inside its own implicit transaction, committed
//     immediately on success or aborted on any exception -- so a caller
//     that never issues BEGIN/COMMIT/ROLLBACK at all still gets SPEC's
//     durability/isolation guarantees (docs/SPEC.md sections 2/3) for
//     every individual statement it runs, exactly the way SQLite's own
//     default "autocommit mode" does. CREATE TABLE/CREATE INDEX are never
//     wrapped in an implicit transaction regardless (docs/SPEC.md section
//     4: no concurrent DDL) -- ExecuteInCurrentTransaction's own guard
//     already rejects DDL run inside any transaction, explicit or
//     implicit, so wrapping it here would make an otherwise-valid CREATE
//     TABLE fail for a reason entirely of this function's own making.
//
// Throws std::logic_error for: BEGIN while this thread already has an
// active transaction (delegated to TransactionManager::Begin()'s own
// check), and COMMIT/ROLLBACK with no active transaction on this thread
// at all (checked here -- see docs/DECISIONS.md D-049 for why
// TransactionManager::Commit()/Abort() cannot be trusted to catch that
// specific case safely on their own). Otherwise throws and propagates
// exactly what ExecuteInCurrentTransaction does.
ExecuteResult Execute(Database& db, const Statement& stmt);

}  // namespace flintdb
