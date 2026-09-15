#include "transaction_manager.h"

#include <stdexcept>

namespace flintdb {

TransactionManager::TransactionManager(BufferPool* buffer_pool, LogManager* log_manager)
    : buffer_pool_(buffer_pool), log_manager_(log_manager) {}

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
    // *this* thread sees txn_ptr as "the" current transaction.
    SetCurrentTransactionForThisThread(txn_ptr);
    buffer_pool_->SetActiveTransactionObserver([txn_ptr](PageId page_id) { txn_ptr->NotifyDirty(page_id); });
    return txn_ptr;
}

void TransactionManager::Commit(Transaction* txn) {
    if (GetCurrentTransaction() != txn) {
        throw std::logic_error(
            "TransactionManager::Commit: txn must be this calling thread's own active transaction (either it "
            "belongs to a different thread, or it was already committed/aborted)");
    }
    buffer_pool_->ClearActiveTransactionObserver();

    for (PageId page_id : txn->DirtiedPages()) {
        Page* page = buffer_pool_->FetchPage(page_id);  // already cached -- just returns it
        log_manager_->AppendUpdate(txn->Id(), page_id, page->Data());
    }
    log_manager_->AppendCommit(txn->Id());
    log_manager_->Flush();                            // fsync the WAL first (write-ahead-logging rule) ...
    buffer_pool_->FlushPages(txn->DirtiedPages());     // ... then force *this transaction's own* pages to
                                                        // catch up (see class comment) -- FlushPages, not
                                                        // FlushAll, so a concurrently-active, still-
                                                        // uncommitted transaction's dirty pages are never
                                                        // swept up by this one's commit (see
                                                        // BufferPool::FlushAll's comment and docs/DECISIONS.md)

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
    buffer_pool_->ClearActiveTransactionObserver();

    for (PageId page_id : txn->DirtiedPages()) {
        buffer_pool_->DiscardPage(page_id);
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
