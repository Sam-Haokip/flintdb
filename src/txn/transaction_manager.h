#pragma once
#include "../storage/buffer_pool.h"
#include "../wal/log_manager.h"
#include "transaction.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace flintdb {

// Coordinates every in-flight transaction's lifecycle against a shared
// BufferPool, LogManager, and LockManager: Begin() opens one, Commit() or
// Abort() closes it out. Phase 4 lifts Phase 3's "exactly one active
// transaction at a time" restriction: multiple transactions, each on its
// own calling thread (the thread-per-transaction model -- see
// Transaction's class comment), can be simultaneously active, coordinated
// through LockManager's Strict 2PL rather than by simply refusing to let
// more than one exist.
//
// What Phase 4 keeps from Phase 3 is a *per-thread* restriction: a thread
// may have at most one active transaction of its own at a time (Begin()
// throws if the calling thread already has one -- see Begin()'s comment),
// because SetCurrentTransactionForThisThread and
// BufferPool::SetActiveTransactionObserver are both keyed by the calling
// thread, so nesting two transactions on the same thread would silently
// overwrite the first's routing rather than actually run them both.
// Different threads each having their own active transaction is exactly
// the concurrency this phase adds.
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
// file before Commit returns, via BufferPool::FlushPages() over exactly
// this transaction's own dirtied pages -- see Commit's comment below for
// why that's FlushPages and not FlushAll as of Phase 4). The WAL fsync
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
// Thread-safe as of Phase 4: mutex_ guards active_txns_ (the map owning
// every currently-active Transaction), and next_txn_id_ is a
// std::atomic so concurrent Begin() calls from different threads always
// get distinct, monotonically increasing ids without needing the same
// mutex -- monotonic assignment matters beyond just uniqueness, since
// LockManager's wait-die deadlock prevention depends on TxnId order
// meaning transaction age (see lock_manager.h).
class TransactionManager {
 public:
    TransactionManager(BufferPool* buffer_pool, LogManager* log_manager);

    // Starts a new transaction, logs a Begin record for it, sets it as
    // this calling thread's current transaction (GetCurrentTransaction(),
    // transaction.h), and registers it as the buffer pool's
    // active-transaction observer *for this thread* so every page it
    // dirties from now on -- through however many HeapFile/B-tree calls
    // happen in between, on this same thread -- is recorded automatically,
    // with neither of those layers needing to know a transaction exists.
    // The returned Transaction* is owned by the TransactionManager and
    // stays valid until the matching Commit() or Abort() call (which must
    // happen on this same thread -- see Commit()/Abort()'s comments).
    //
    // Throws std::logic_error if the calling thread already has an
    // active transaction of its own -- see the class comment. Different
    // threads calling Begin() concurrently is the supported, tested case.
    Transaction* Begin();

    // Logs an Update record (the page's current in-memory content, read
    // back via BufferPool::FetchPage) for every page `txn` dirtied, then
    // a Commit record, then calls LogManager::Flush() (fsync -- this is
    // what actually makes the commit durable) and finally
    // BufferPool::FlushPages(txn->DirtiedPages()) (the "force" half of
    // the policy described in the class comment, in that order) --
    // deliberately FlushPages, scoped to this transaction's own pages,
    // rather than FlushAll: under Phase 4's real concurrency, FlushAll
    // would also flush a different, concurrently-active transaction's
    // still-uncommitted dirty pages, which is a no-steal violation (see
    // BufferPool::FlushAll's own comment and docs/DECISIONS.md for how
    // this was caught, during Phase 4 design). Finally releases every
    // lock `txn` holds via LockManager::ReleaseAll -- only *after* the
    // flush, which is what makes this Strict 2PL (locks held through
    // durability, not just through the WAL append) -- clears the buffer
    // pool's observer and this thread's current-transaction pointer, and
    // removes `txn` from the active set.
    //
    // `txn` must be this calling thread's current transaction (i.e. the
    // pointer this same thread's own Begin() returned, not yet committed
    // or aborted) -- anything else, including a transaction that's valid
    // but belongs to a *different* thread, is a std::logic_error. This is
    // stricter than just "some transaction somewhere is still active":
    // it's what keeps this thread's own thread-local context and
    // per-thread observer registration correct, since those are only
    // ever meaningful for the thread that owns them (see transaction.h).
    void Commit(Transaction* txn);

    // Reverts every page `txn` dirtied back to its on-disk content (via
    // BufferPool::DiscardPage -- safe to do because Commit's force-flush
    // guarantees the data file always reflects at least "as of the last
    // commit", see the class comment) and logs a diagnostic Abort record
    // -- recovery never looks for it, since a transaction with no Commit
    // record is already correctly ignored either way (see
    // wal/recovery.h). Releases every lock `txn` holds via
    // LockManager::ReleaseAll, clears the buffer pool's observer and this
    // thread's current-transaction pointer, and removes `txn` from the
    // active set. Same "txn must be this thread's own active transaction"
    // requirement as Commit.
    void Abort(Transaction* txn);

    // Whether at least one transaction is currently active, across all
    // threads. Exposed for tests and for a future caller (e.g. Phase 5's
    // SQL executor, or a checkpoint scheduler that must wait for
    // quiescence -- see LogManager::Checkpoint) that wants to check
    // before an operation that isn't safe with any transaction in flight.
    bool HasActiveTransaction() const;

    // How many transactions are currently active, across all threads.
    // Exposed for tests that need to assert real concurrent overlap (more
    // than one active at the same instant), which HasActiveTransaction()
    // alone can't distinguish from exactly one.
    size_t NumActiveTransactions() const;

 private:
    BufferPool* buffer_pool_;
    LogManager* log_manager_;
    // Owned here (composition, not injected like buffer_pool_/log_manager_
    // above) because a LockManager's lifetime is naturally scoped to "the
    // set of transactions this TransactionManager coordinates" -- see
    // lock_manager.h's class comment ("shared by every Transaction in one
    // TransactionManager's scope") -- and nothing else in the system
    // needs a reference to it.
    LockManager lock_manager_;

    mutable std::mutex mutex_;
    std::unordered_map<TxnId, std::unique_ptr<Transaction>> active_txns_;
    std::atomic<TxnId> next_txn_id_{1};
};

}  // namespace flintdb
