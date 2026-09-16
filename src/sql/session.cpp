#include "session.h"

#include "../txn/transaction.h"

#include <stdexcept>
#include <variant>

namespace flintdb {

namespace {

ExecuteResult ExecuteBegin(Database& db) {
    // TransactionManager::Begin() already throws std::logic_error if this
    // thread has an active transaction of its own -- no need to duplicate
    // that check here.
    db.GetTransactionManager().Begin();
    return ExecuteResult{};
}

ExecuteResult ExecuteCommit(Database& db) {
    Transaction* txn = GetCurrentTransaction();
    // docs/DECISIONS.md D-049: TransactionManager::Commit(txn) only
    // throws when `txn` belongs to a different transaction than the one
    // currently active on this thread (GetCurrentTransaction() != txn) --
    // if this thread has no active transaction at all, GetCurrentTransaction()
    // is nullptr, and Commit(nullptr) would pass that check trivially
    // (nullptr != nullptr is false) and go on to dereference a null txn
    // deeper inside. This check must happen here, not inside
    // TransactionManager, which has no way to distinguish "no active
    // transaction" from "the caller correctly passed its own".
    if (txn == nullptr) {
        throw std::logic_error("COMMIT: no active transaction on this thread");
    }
    db.GetTransactionManager().Commit(txn);
    return ExecuteResult{};
}

ExecuteResult ExecuteRollback(Database& db) {
    Transaction* txn = GetCurrentTransaction();
    // Same reasoning as ExecuteCommit above.
    if (txn == nullptr) {
        throw std::logic_error("ROLLBACK: no active transaction on this thread");
    }
    db.GetTransactionManager().Abort(txn);
    return ExecuteResult{};
}

}  // namespace

ExecuteResult Execute(Database& db, const Statement& stmt) {
    if (std::holds_alternative<BeginStatement>(stmt)) return ExecuteBegin(db);
    if (std::holds_alternative<CommitStatement>(stmt)) return ExecuteCommit(db);
    if (std::holds_alternative<RollbackStatement>(stmt)) return ExecuteRollback(db);

    if (std::holds_alternative<CreateTableStatement>(stmt) || std::holds_alternative<CreateIndexStatement>(stmt)) {
        // DDL is never wrapped in an implicit transaction -- see this
        // function's own doc comment (session.h) for why: it would only
        // make ExecuteInCurrentTransaction's own "no concurrent DDL"
        // guard reject it.
        return ExecuteInCurrentTransaction(db, stmt);
    }

    // INSERT/SELECT/UPDATE/DELETE: docs/DECISIONS.md D-049's autocommit
    // policy. If this thread already has an explicit transaction open
    // (a prior BEGIN, not yet COMMIT/ROLLBACK-ed), this statement simply
    // joins it -- ExecuteInCurrentTransaction already runs under whatever
    // context GetCurrentTransaction() reports, so no special handling is
    // needed beyond *not* wrapping it in a second, nested transaction
    // (which TransactionManager::Begin() would refuse anyway).
    if (GetCurrentTransaction() != nullptr) {
        return ExecuteInCurrentTransaction(db, stmt);
    }

    // No transaction open: wrap this one statement in its own implicit
    // transaction, committed on success or aborted on any exception --
    // SPEC's autocommit default (D-049).
    Transaction* txn = db.GetTransactionManager().Begin();
    try {
        ExecuteResult result = ExecuteInCurrentTransaction(db, stmt);
        db.GetTransactionManager().Commit(txn);
        return result;
    } catch (...) {
        db.GetTransactionManager().Abort(txn);
        throw;
    }
}

}  // namespace flintdb
