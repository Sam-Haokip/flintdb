#include "transaction_manager.h"

#include <stdexcept>

namespace flintdb {

TransactionManager::TransactionManager(LogManager* log_manager) : log_manager_(log_manager) {}

void TransactionManager::RegisterObject(ObjectId object_id, BufferPool* buffer_pool) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_pools_[object_id] = buffer_pool;
}

std::unordered_map<ObjectId, std::set<PageId>> TransactionManager::GroupDirtiedPagesByObject(
    const Transaction* txn) const {
    std::unordered_map<ObjectId, std::set<PageId>> by_object;
    for (const PageKey& key : txn->DirtiedPages()) {
        by_object[key.object_id].insert(key.page_id);
    }
    return by_object;
}

Transaction* TransactionManager::Begin() {
    if (GetCurrentTransaction() != nullptr) {
        throw std::logic_error(
            "TransactionManager::Begin: this thread already has an active transaction -- Phase 4 allows one "
            "transaction per thread at a time (not nested/overlapping transactions on the same thread), though "
            "different threads may each have their own active transaction simultaneously");
    }

    TxnId txn_id = next_txn_id_.fetch_add(1);
    log_manager_->AppendBegin(txn_id);

    auto txn = std::make_unique<Transaction>(txn_id, &lock_manager_);
    Transaction* txn_ptr = txn.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_txns_[txn_id] = std::move(txn);
    }

    // Both of these are keyed by the calling thread -- see transaction.h
    // and buffer_pool.h's class comments -- so from this point on, only
    // *this* thread sees txn_ptr as "the" current transaction. Every
    // currently-registered object gets its own observer, each one closing
    // over *its own* object_id so NotifyDirty always records which object
    // a page belongs to (docs/DECISIONS.md D-032/D-035) -- copy the
    // registry under mutex_ first so this loop doesn't hold that lock
    // while calling into each BufferPool.
    SetCurrentTransactionForThisThread(txn_ptr);
    std::unordered_map<ObjectId, BufferPool*> pools_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_snapshot = buffer_pools_;
    }
    for (const auto& [object_id, pool] : pools_snapshot) {
        pool->SetActiveTransactionObserver(
            [txn_ptr, object_id](PageId page_id) { txn_ptr->NotifyDirty(object_id, page_id); });
    }
    return txn_ptr;
}

void TransactionManager::Commit(Transaction* txn) {
    if (GetCurrentTransaction() != txn) {
        throw std::logic_error(
            "TransactionManager::Commit: txn must be this calling thread's own active transaction (either it "
            "belongs to a different thread, or it was already committed/aborted)");
    }

    std::unordered_map<ObjectId, BufferPool*> pools_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_snapshot = buffer_pools_;
    }
    for (const auto& [object_id, pool] : pools_snapshot) {
        pool->ClearActiveTransactionObserver();
    }

    std::unordered_map<ObjectId, std::set<PageId>> by_object = GroupDirtiedPagesByObject(txn);
    for (const auto& [object_id, page_ids] : by_object) {
        BufferPool* pool = pools_snapshot.at(object_id);  // must already be registered -- see RegisterObject
        for (PageId page_id : page_ids) {
            Page* page = pool->FetchPage(page_id);  // already cached -- just returns it
            log_manager_->AppendUpdate(txn->Id(), object_id, page_id, page->Data());
        }
    }
    Lsn commit_lsn = log_manager_->AppendCommit(txn->Id());
    // fsync the WAL first (write-ahead-logging rule) -- via FlushThrough,
    // not a plain Flush(), so concurrent commits coalesce into fewer
    // fsync calls instead of each paying its own fsync latency while
    // holding every lock this transaction still holds (Strict 2PL: all
    // locks held until commit, including BPlusTree's root-lock sentinel,
    // D-028) -- see docs/DECISIONS.md D-055 for the benchmark finding
    // that motivated this and log_manager.h's FlushThrough for the full
    // group-commit protocol.
    log_manager_->FlushThrough(commit_lsn);
    for (const auto& [object_id, page_ids] : by_object) {
        // ... then force *this transaction's own* pages, per object, to
        // catch up (see class comment) -- FlushPages, not FlushAll, so a
        // concurrently-active, still-uncommitted transaction's dirty
        // pages are never swept up by this one's commit (see
        // BufferPool::FlushAll's comment and docs/DECISIONS.md).
        pools_snapshot.at(object_id)->FlushPages(page_ids);
    }

    lock_manager_.ReleaseAll(txn->Id());  // Strict 2PL: release only now, after durability, never earlier
    SetCurrentTransactionForThisThread(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    active_txns_.erase(txn->Id());  // destroys the Transaction -- txn is a dangling pointer after this returns
}

void TransactionManager::Abort(Transaction* txn) {
    if (GetCurrentTransaction() != txn) {
        throw std::logic_error(
            "TransactionManager::Abort: txn must be this calling thread's own active transaction (either it "
            "belongs to a different thread, or it was already committed/aborted)");
    }

    std::unordered_map<ObjectId, BufferPool*> pools_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_snapshot = buffer_pools_;
    }
    for (const auto& [object_id, pool] : pools_snapshot) {
        pool->ClearActiveTransactionObserver();
    }

    std::unordered_map<ObjectId, std::set<PageId>> by_object = GroupDirtiedPagesByObject(txn);
    for (const auto& [object_id, page_ids] : by_object) {
        BufferPool* pool = pools_snapshot.at(object_id);
        for (PageId page_id : page_ids) {
            pool->DiscardPage(page_id);
        }
    }
    log_manager_->AppendAbort(txn->Id());

    lock_manager_.ReleaseAll(txn->Id());
    SetCurrentTransactionForThisThread(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    active_txns_.erase(txn->Id());
}

bool TransactionManager::HasActiveTransaction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_txns_.empty();
}

size_t TransactionManager::NumActiveTransactions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_txns_.size();
}

}  // namespace flintdb
