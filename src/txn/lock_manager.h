#pragma once
#include "../common/config.h"
#include "../wal/log_record.h"  // for TxnId

#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace flintdb {

enum class LockMode { kShared, kExclusive };

// Thrown by LockManager::AcquireLock when the requesting transaction is
// chosen as a wait-die "victim" -- it must abort (see the class comment
// below for the full rule). Whoever calls AcquireLock (directly, or via
// Transaction::AcquireLock -- see transaction.h) is expected to catch
// this, call TransactionManager::Abort on the transaction, and decide
// whether to retry; LockManager itself never retries or blocks past this
// point for a doomed request.
class TransactionAbortedException : public std::runtime_error {
 public:
    explicit TransactionAbortedException(TxnId txn_id)
        : std::runtime_error("transaction " + std::to_string(txn_id) +
                              " aborted by the lock manager (wait-die: a younger transaction "
                              "conflicted with an older one and was chosen as the victim)"),
          txn_id(txn_id) {}
    TxnId txn_id;
};

// The Strict Two-Phase Locking lock table (see docs/SPEC.md section 3):
// shared/exclusive locks at page granularity, acquired freely during a
// transaction's growing phase and released all at once at commit or
// abort (ReleaseAll) -- never one at a time, which is what makes this
// "strict" 2PL rather than plain 2PL, and is also exactly what makes
// deadlock possible in the first place (a transaction can end up holding
// a lock another transaction needs while waiting for a third).
//
// Deadlocks are *prevented*, not detected after the fact, using wait-die:
// transaction ids are assigned monotonically increasing by
// TransactionManager, so a smaller TxnId means an older transaction.
// When Ti requests a lock currently held (incompatibly) by one or more
// other transactions, Ti may wait only if it is older than *every*
// conflicting holder; if it is younger than even one of them, it dies
// (AcquireLock throws TransactionAbortedException immediately, no
// blocking). This is provably deadlock-free: a transaction only ever
// waits for strictly older transactions, so no cycle of "waits for" can
// ever close, without needing a wait-for graph or periodic cycle
// detection. It only ever aborts the transaction making the *new*
// request, on its own calling thread -- never a transaction actively
// running elsewhere, which is what a preemptive scheme like wound-wait
// would require (and which this project deliberately avoids -- see
// docs/DECISIONS.md D-022).
//
// This is a purely logical lock table, independent of the buffer pool's
// own (short-lived, physical) internal mutex -- see BufferPool's class
// comment for that distinction. A LockManager instance is meant to be
// shared by every Transaction in one TransactionManager's scope.
class LockManager {
 public:
    LockManager() = default;

    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // Acquires `mode` on `page_id` for `txn_id`. Returns immediately if
    // `txn_id` already holds a sufficient lock on this page (including
    // re-requesting the same mode, or requesting shared while already
    // holding exclusive). Upgrades in place (shared -> exclusive) if
    // `txn_id` is the page's only shared holder and no one else holds it.
    // Otherwise blocks until the lock is free, or throws
    // TransactionAbortedException per the wait-die rule described above
    // -- which happens without blocking at all, so a call to this never
    // both throws and waits.
    void AcquireLock(TxnId txn_id, PageId page_id, LockMode mode);

    // Releases every lock `txn_id` currently holds, all at once (Strict
    // 2PL never releases a lock before end of transaction). Safe to call
    // on a transaction holding no locks at all. Wakes up every thread
    // waiting on any page this freed, so they can re-check whether they
    // can now proceed.
    void ReleaseAll(TxnId txn_id);

    // Whether `txn_id` currently holds any lock at all -- for tests and
    // diagnostics only, not used by the acquire/release logic itself.
    bool HasAnyLock(TxnId txn_id) const;

 private:
    struct LockEntry {
        std::set<TxnId> shared_holders;
        std::optional<TxnId> exclusive_holder;
    };

    // True if `txn_id` requesting `mode` on `entry` would conflict with
    // any *other* current holder (its own existing hold on this page, if
    // any, is never a conflict with itself).
    bool ConflictsWithOthers(const LockEntry& entry, TxnId txn_id, LockMode mode) const;

    // Every current holder of `entry` other than `txn_id` itself -- the
    // set wait-die's older-than-every-holder check is evaluated against.
    std::set<TxnId> OtherHolders(const LockEntry& entry, TxnId txn_id) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<PageId, LockEntry> locks_;
    std::unordered_map<TxnId, std::set<PageId>> held_by_txn_;
};

}  // namespace flintdb
