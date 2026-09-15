#pragma once
#include "../common/config.h"
#include "../wal/log_record.h"

#include <set>

namespace flintdb {

// Tracks exactly one in-flight transaction's state: its id, and which
// pages it has dirtied so far (TransactionManager::Commit needs that set
// to know which pages to write Update records for; Abort needs it to know
// which pages to revert).
//
// Phase 3 keeps this deliberately minimal: no read set, no lock table, no
// isolation guarantees beyond "TransactionManager only ever lets one of
// these be active at a time" (see transaction_manager.h). Real
// concurrency -- multiple simultaneous transactions, locking or MVCC,
// isolation levels -- is Phase 4's job, per docs/SPEC.md section 3.
class Transaction {
 public:
    explicit Transaction(TxnId txn_id) : txn_id_(txn_id) {}

    TxnId Id() const { return txn_id_; }

    // Called by BufferPool's active-transaction observer (wired up in
    // TransactionManager::Begin) every time this transaction's work marks
    // a page dirty. Recording the same page more than once is harmless --
    // dirtied_pages_ is a set, and only the page's *final* content at
    // commit time ends up in the WAL either way (see
    // TransactionManager::Commit).
    void NotifyDirty(PageId page_id) { dirtied_pages_.insert(page_id); }

    // Every page this transaction has dirtied so far, in page-id order.
    // Order isn't load-bearing for correctness (each Update record names
    // its own page_id), but a deterministic iteration order keeps tests
    // and any future debug logging reproducible for free.
    const std::set<PageId>& DirtiedPages() const { return dirtied_pages_; }

 private:
    TxnId txn_id_;
    std::set<PageId> dirtied_pages_;
};

}  // namespace flintdb
