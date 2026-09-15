#pragma once
#include "../storage/buffer_pool.h"
#include "../wal/log_manager.h"
#include "transaction.h"

#include <memory>

namespace flintdb {

// Coordinates a single in-flight transaction's lifecycle against a
// BufferPool and a LogManager: Begin() opens one, Commit() or Abort()
// closes it out. Phase 3 supports exactly one active transaction at a
// time -- see Transaction's class comment and docs/SPEC.md section 3 for
// why real concurrency (overlapping transactions) waits for Phase 4.
//
// This is what actually makes FlintDB's durability guarantee (SPEC.md
// section 2) real rather than aspirational: Commit logs a whole-page-image
// Update record for every page the transaction dirtied, then a Commit
// record, then fsyncs the log, THEN flushes those pages to the data file
// -- in that order -- before returning, so "Commit() returned" and "this
// transaction's changes survive a crash" become the same fact (see
// docs/SPEC.md section 5's verification standard, and the crash-simulation
// tests in tests/test_transaction.cpp that actually check this rather than
// asserting it by inspection).
//
// That's a no-steal *and* force buffer-management policy: no-steal (an
// uncommitted page is never written to the data file -- see
// wal/recovery.h) plus force (a committed page *is* written to the data
// file before Commit returns, via BufferPool::FlushAll()). The WAL fsync
// has to happen before that flush, not after -- the write-ahead-logging
// rule -- so that a crash between the two still leaves the WAL as the
// sole source of truth for whether the transaction committed, with
// RunRecovery able to finish the flush that didn't get to happen.
//
// Force was chosen over the more common lazy/no-force alternative (commit
// is just a WAL append + fsync; the data file catches up later, at the
// next checkpoint or eviction) for a specific correctness reason, not
// performance: Abort reverts a page by re-reading it from the data file
// (BufferPool::DiscardPage), and Phase 3's BufferPool mutates pages in
// place with no copy-on-write, so there is no other "before image" to
// revert to. Under no-force, an earlier transaction's committed change
// could still exist only in the WAL (not yet flushed) when a *later*
// transaction touches the same page and then aborts -- DiscardPage would
// reload the data file's stale pre-commit content and silently erase the
// earlier commit from memory (a real crash right after would still
// recover correctly from the WAL, but a process that keeps running would
// now be serving wrong data). Force closes that gap for free: every
// commit leaves the data file caught up to "as of the last commit," so
// DiscardPage always has a safe, correct state to revert to. See
// docs/DECISIONS.md for the full writeup and what forcing costs (a
// synchronous data-file write on every commit, not just a log append) --
// revisit in Phase 7 if that cost matters against a real workload.
//
// Not thread-safe -- concurrent access is a Phase 4 concern layered above
// this, same as BufferPool, DiskManager, and LogManager.
class TransactionManager {
 public:
    TransactionManager(BufferPool* buffer_pool, LogManager* log_manager);

    // Starts a new transaction, logs a Begin record for it, and registers
    // it as the buffer pool's active-transaction observer so every page
    // it dirties from now on -- through however many HeapFile/B-tree
    // calls happen in between -- is recorded automatically, with neither
    // of those layers needing to know a transaction exists. The returned
    // Transaction* is owned by the TransactionManager and stays valid
    // until the matching Commit() or Abort() call.
    //
    // Throws std::logic_error if a transaction is already active -- see
    // the class comment on why Phase 3 doesn't support overlapping ones.
    Transaction* Begin();

    // Logs an Update record (the page's current in-memory content, read
    // back via BufferPool::FetchPage) for every page `txn` dirtied, then
    // a Commit record, then calls LogManager::Flush() (fsync -- this is
    // what actually makes the commit durable) and finally
    // BufferPool::FlushAll() (the "force" half of the policy described
    // in the class comment, in that order). Clears the buffer pool's
    // observer and ends the active transaction.
    //
    // `txn` must be the currently active transaction (the pointer
    // Begin() returned); anything else is a std::logic_error, since
    // Phase 3 never has more than one to pass.
    void Commit(Transaction* txn);

    // Reverts every page `txn` dirtied back to its on-disk content (via
    // BufferPool::DiscardPage -- safe to do because Commit's force-flush
    // guarantees the data file always reflects at least "as of the last
    // commit", see the class comment) and logs a diagnostic Abort record
    // -- recovery never looks for it, since a transaction with no Commit
    // record is already correctly ignored either way (see
    // wal/recovery.h). Clears the buffer pool's observer and ends the
    // active transaction. Same "txn must be active" requirement as
    // Commit.
    void Abort(Transaction* txn);

    // Whether a transaction is currently active. Exposed for tests and
    // for a future caller (e.g. Phase 5's SQL executor) that wants to
    // check before trying to Begin() rather than catching the exception.
    bool HasActiveTransaction() const;

 private:
    BufferPool* buffer_pool_;
    LogManager* log_manager_;
    std::unique_ptr<Transaction> active_txn_;
    TxnId next_txn_id_ = 1;
};

}  // namespace flintdb
