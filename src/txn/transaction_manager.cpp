#include "transaction_manager.h"

#include <stdexcept>

namespace flintdb {

TransactionManager::TransactionManager(BufferPool* buffer_pool, LogManager* log_manager)
    : buffer_pool_(buffer_pool), log_manager_(log_manager) {}

Transaction* TransactionManager::Begin() {
    if (active_txn_) {
        throw std::logic_error(
            "TransactionManager::Begin: a transaction is already active (Phase 3 supports only one at a time)");
    }
    TxnId txn_id = next_txn_id_++;
    log_manager_->AppendBegin(txn_id);
    active_txn_ = std::make_unique<Transaction>(txn_id);

    Transaction* txn = active_txn_.get();
    buffer_pool_->SetActiveTransactionObserver([txn](PageId page_id) { txn->NotifyDirty(page_id); });
    return txn;
}

void TransactionManager::Commit(Transaction* txn) {
    if (!active_txn_ || txn != active_txn_.get()) {
        throw std::logic_error("TransactionManager::Commit: txn is not the active transaction");
    }
    buffer_pool_->ClearActiveTransactionObserver();

    for (PageId page_id : txn->DirtiedPages()) {
        Page* page = buffer_pool_->FetchPage(page_id);  // already cached -- just returns it
        log_manager_->AppendUpdate(txn->Id(), page_id, page->Data());
    }
    log_manager_->AppendCommit(txn->Id());
    log_manager_->Flush();       // fsync the WAL first (write-ahead-logging rule) ...
    buffer_pool_->FlushAll();    // ... then force the data file to catch up (see class comment)

    active_txn_.reset();
}

void TransactionManager::Abort(Transaction* txn) {
    if (!active_txn_ || txn != active_txn_.get()) {
        throw std::logic_error("TransactionManager::Abort: txn is not the active transaction");
    }
    buffer_pool_->ClearActiveTransactionObserver();

    for (PageId page_id : txn->DirtiedPages()) {
        buffer_pool_->DiscardPage(page_id);
    }
    log_manager_->AppendAbort(txn->Id());

    active_txn_.reset();
}

bool TransactionManager::HasActiveTransaction() const { return active_txn_ != nullptr; }

}  // namespace flintdb
