#pragma once
#include "../common/config.h"
#include "../wal/log_record.h"
#include "lock_manager.h"

#include <set>

namespace flintdb {

// Tracks one in-flight transaction's state: its id, which pages it has
// dirtied so far (TransactionManager::Commit needs that set to know
// which pages to write Update records for; Abort needs it to know which
// pages to revert), and -- as of Phase 4 -- the shared LockManager it
// acquires page locks through for Strict 2PL.
//
// Phase 3 allowed only one of these to be active at a time; Phase 4 lifts
// that restriction (see transaction_manager.h) and gives each Transaction
// its own std::thread to run on (the "thread-per-transaction" model this
// project uses to get genuine, not simulated, concurrency -- see
// docs/DECISIONS.md). A Transaction object itself still has no locking
// around its own state (dirtied_pages_ in particular): that's safe
// precisely *because* of thread-per-transaction -- exactly one thread
// (the one running this transaction) ever calls NotifyDirty or
// AcquireLock on a given Transaction instance, so there is no concurrent
// access to protect against from this class's own point of view. Two
// different Transaction instances used from two different threads is the
// supported (and tested) case; one Transaction instance used from two
// threads at once is not a pattern this project supports (see
// docs/SPEC.md section 4's non-goals).
class Transaction {
 public:
    Transaction(TxnId txn_id, LockManager* lock_manager) : txn_id_(txn_id), lock_manager_(lock_manager) {}

    // A safety net, not part of Strict 2PL itself: if this object is
    // destroyed while the calling (destroying) thread's thread-local
    // "current transaction" still points to it -- which happens when a
    // transaction is abandoned without Commit()/Abort() ever running
    // (TransactionManager::Commit/Abort already clear this themselves
    // before erasing, in the normal path) -- clear that pointer so
    // nothing can later dereference it. This matters even though a real
    // crash would take the whole process (and its thread-local storage)
    // down with it: a *simulated* crash in a test, or any other case
    // where the process keeps running after a transaction is abandoned,
    // would otherwise leave a dangling pointer for the next thing that
    // calls GetCurrentTransaction() on this thread to use-after-free.
    // Only clears this thread's own slot -- see the class comment above
    // on why cross-thread teardown isn't a pattern this project supports.
    ~Transaction();

    TxnId Id() const { return txn_id_; }

    // Called by BufferPool's per-thread transaction observer (wired up in
    // TransactionManager::Begin) every time this transaction's work marks
    // a page dirty. Recording the same (object, page) more than once is
    // harmless -- dirtied_pages_ is a set, and only the page's *final*
    // content at commit time ends up in the WAL either way (see
    // TransactionManager::Commit). Qualified by ObjectId as of Phase 5,
    // since a transaction can dirty pages across more than one table/index
    // in the same Database (docs/DECISIONS.md D-032) and each is only
    // fetchable through *that* object's own BufferPool.
    void NotifyDirty(ObjectId object_id, PageId page_id) { dirtied_pages_.insert(PageKey{object_id, page_id}); }

    // Every (object, page) this transaction has dirtied so far, in sorted
    // order. Order isn't load-bearing for correctness (each Update record
    // names its own object_id/page_id), but a deterministic iteration
    // order keeps tests and any future debug logging reproducible for
    // free.
    const std::set<PageKey>& DirtiedPages() const { return dirtied_pages_; }

    // Acquires `mode` on `page_id` (within `object_id`'s own file) through
    // this transaction's shared LockManager, on behalf of this
    // transaction's id -- the Strict 2PL entry point every page-touching
    // HeapFile/BTree call site goes through once locking is wired in (see
    // GetCurrentTransaction() below and docs/SPEC.md section 3). Blocks
    // per wait-die, or throws TransactionAbortedException if this
    // transaction is chosen as the victim (see lock_manager.h) -- callers
    // are expected to let that propagate up to whoever is driving the
    // transaction, which must catch it and call TransactionManager::Abort
    // rather than continue using a transaction that just lost a lock
    // request it needed.
    //
    // A null lock_manager_ (never true for a Transaction TransactionManager
    // constructs, but kept safe for tests that construct one standalone)
    // makes this a no-op, consistent with GetCurrentTransaction()
    // returning nullptr being the "no transaction, no locking" case
    // everywhere else in this design.
    void AcquireLock(ObjectId object_id, PageId page_id, LockMode mode) {
        if (lock_manager_) lock_manager_->AcquireLock(txn_id_, object_id, page_id, mode);
    }

 private:
    TxnId txn_id_;
    LockManager* lock_manager_;
    std::set<PageKey> dirtied_pages_;
};

// Thread-local "which transaction, if any, is running on this thread"
// context. HeapFile/BTree call GetCurrentTransaction() at each
// page-touching call site (once Phase 4 wires that in -- see
// docs/SPEC.md section 3) to decide whether to acquire a lock at all and,
// if so, through which transaction; nullptr means no transaction is
// active on this thread, which is the case for every pre-Phase-4 test and
// any future caller that uses HeapFile/BTree directly without going
// through TransactionManager -- both must keep working exactly as
// before, unlocked, which is exactly what "no-op when
// GetCurrentTransaction() is nullptr" gives for free.
//
// This lives in src/txn, not src/storage, specifically so BufferPool
// never has to depend on Transaction/TransactionManager -- see
// buffer_pool.h's class comment on its thread-keyed observer registry,
// which is the other half of this same decoupling strategy applied to
// the opposite direction of dependency. src/storage's HeapFile/BTree are
// free to depend on src/txn (which does not depend back on them), so
// having them call this creates no circular dependency between
// src/storage and src/txn.
Transaction* GetCurrentTransaction();

// Sets (or clears, with nullptr) the transaction considered "current" for
// the calling thread. Called by TransactionManager::Begin (on the thread
// that called Begin) and by Commit/Abort (to clear it again on the same
// thread) -- see transaction_manager.cpp. HeapFile/BTree never call this,
// only GetCurrentTransaction(); this asymmetry is deliberate, since only
// TransactionManager is meant to decide what "the current transaction"
// means for a thread.
void SetCurrentTransactionForThisThread(Transaction* txn);

}  // namespace flintdb
