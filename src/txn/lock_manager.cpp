#include "lock_manager.h"

namespace flintdb {

bool LockManager::ConflictsWithOthers(const LockEntry& entry, TxnId txn_id, LockMode mode) const {
    if (mode == LockMode::kShared) {
        return entry.exclusive_holder.has_value() && *entry.exclusive_holder != txn_id;
    }
    // kExclusive conflicts with any *other* holder, shared or exclusive.
    if (entry.exclusive_holder.has_value() && *entry.exclusive_holder != txn_id) return true;
    for (TxnId holder : entry.shared_holders) {
        if (holder != txn_id) return true;
    }
    return false;
}

std::set<TxnId> LockManager::OtherHolders(const LockEntry& entry, TxnId txn_id) const {
    std::set<TxnId> others;
    if (entry.exclusive_holder.has_value() && *entry.exclusive_holder != txn_id) {
        others.insert(*entry.exclusive_holder);
    }
    for (TxnId holder : entry.shared_holders) {
        if (holder != txn_id) others.insert(holder);
    }
    return others;
}

void LockManager::AcquireLock(TxnId txn_id, PageId page_id, LockMode mode) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        auto it = locks_.find(page_id);
        if (it == locks_.end()) {
            // Nobody holds this page at all -- grant immediately, no
            // conflict possible.
            LockEntry& fresh = locks_[page_id];
            if (mode == LockMode::kExclusive) {
                fresh.exclusive_holder = txn_id;
            } else {
                fresh.shared_holders.insert(txn_id);
            }
            held_by_txn_[txn_id].insert(page_id);
            return;
        }

        LockEntry& entry = it->second;

        // Already holds a sufficient lock? (Including re-requesting the
        // same page, which every mutating HeapFile/B-tree call site that
        // touches a page more than once during one operation will do.)
        bool already_exclusive = entry.exclusive_holder.has_value() && *entry.exclusive_holder == txn_id;
        if (already_exclusive) return;
        if (mode == LockMode::kShared && entry.shared_holders.count(txn_id) > 0) return;

        if (!ConflictsWithOthers(entry, txn_id, mode)) {
            // Grant -- including an in-place shared-to-exclusive upgrade
            // when txn_id already held shared and was the only holder.
            if (mode == LockMode::kExclusive) {
                entry.shared_holders.erase(txn_id);
                entry.exclusive_holder = txn_id;
            } else {
                entry.shared_holders.insert(txn_id);
            }
            held_by_txn_[txn_id].insert(page_id);
            return;
        }

        // Conflict: wait-die. txn_id may wait only if it is older
        // (smaller TxnId) than *every* other current holder; otherwise
        // it dies immediately rather than blocking at all.
        std::set<TxnId> others = OtherHolders(entry, txn_id);
        bool older_than_all = true;
        for (TxnId holder : others) {
            if (!(txn_id < holder)) {
                older_than_all = false;
                break;
            }
        }
        if (older_than_all) {
            cv_.wait(lock);
            continue;  // re-check everything from the top after waking
        }
        throw TransactionAbortedException(txn_id);
    }
}

void LockManager::ReleaseAll(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = held_by_txn_.find(txn_id);
    if (it == held_by_txn_.end()) return;

    for (PageId page_id : it->second) {
        auto lock_it = locks_.find(page_id);
        if (lock_it == locks_.end()) continue;
        LockEntry& entry = lock_it->second;
        entry.shared_holders.erase(txn_id);
        if (entry.exclusive_holder.has_value() && *entry.exclusive_holder == txn_id) {
            entry.exclusive_holder.reset();
        }
        if (entry.shared_holders.empty() && !entry.exclusive_holder.has_value()) {
            locks_.erase(lock_it);  // keeps the table bounded by "currently held", not "ever touched"
        }
    }
    held_by_txn_.erase(it);
    cv_.notify_all();  // wake every waiter so each can re-check its own request
}

bool LockManager::HasAnyLock(TxnId txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = held_by_txn_.find(txn_id);
    return it != held_by_txn_.end() && !it->second.empty();
}

}  // namespace flintdb
