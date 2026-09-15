#include "../src/txn/transaction.h"
#include "test_framework.h"

#include <thread>
#include <vector>

using namespace flintdb;

FLINTDB_TEST(transaction_context_defaults_to_null_with_no_transaction_set) {
    // A fresh thread (this test itself, and every pre-Phase-4 caller)
    // must see "no transaction active" by default -- that's what makes
    // GetCurrentTransaction() safe to call unconditionally from
    // HeapFile/BTree without breaking every non-transactional test.
    FLINTDB_CHECK(GetCurrentTransaction() == nullptr);
}

FLINTDB_TEST(transaction_context_set_and_clear_round_trips) {
    LockManager lm;
    Transaction txn(1, &lm);

    SetCurrentTransactionForThisThread(&txn);
    FLINTDB_CHECK(GetCurrentTransaction() == &txn);

    SetCurrentTransactionForThisThread(nullptr);
    FLINTDB_CHECK(GetCurrentTransaction() == nullptr);
}

FLINTDB_TEST(transaction_context_is_independent_per_thread) {
    // The core thread-per-transaction guarantee: two threads setting
    // different "current transaction" pointers must never see each
    // other's, and the calling thread's own context (never set here)
    // must be unaffected by either background thread.
    LockManager lm;
    Transaction txn_a(1, &lm);
    Transaction txn_b(2, &lm);

    std::vector<Transaction*> seen_by_thread(2, nullptr);
    std::thread ta([&] {
        SetCurrentTransactionForThisThread(&txn_a);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // give tb a chance to interleave
        seen_by_thread[0] = GetCurrentTransaction();
    });
    std::thread tb([&] {
        SetCurrentTransactionForThisThread(&txn_b);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        seen_by_thread[1] = GetCurrentTransaction();
    });
    ta.join();
    tb.join();

    FLINTDB_CHECK(seen_by_thread[0] == &txn_a);
    FLINTDB_CHECK(seen_by_thread[1] == &txn_b);
    FLINTDB_CHECK(GetCurrentTransaction() == nullptr);  // this (main) thread was never touched
}

FLINTDB_TEST(transaction_acquire_lock_is_a_no_op_with_a_null_lock_manager) {
    Transaction txn(1, nullptr);
    txn.AcquireLock(100, LockMode::kExclusive);  // must not throw or crash
}

FLINTDB_TEST(transaction_acquire_lock_routes_through_the_shared_lock_manager) {
    LockManager lm;
    Transaction txn(7, &lm);
    txn.AcquireLock(100, LockMode::kExclusive);
    FLINTDB_CHECK(lm.HasAnyLock(7));
}

FLINTDB_TEST(transaction_acquire_lock_propagates_a_wait_die_abort_as_transaction_aborted_exception) {
    LockManager lm;
    TxnId older = 1;
    TxnId younger = 2;
    lm.AcquireLock(older, 100, LockMode::kExclusive);  // held directly, simulating another transaction

    Transaction younger_txn(younger, &lm);
    bool threw = false;
    try {
        younger_txn.AcquireLock(100, LockMode::kExclusive);  // must die, not wait -- younger than older
    } catch (const TransactionAbortedException& e) {
        threw = true;
        FLINTDB_CHECK_EQ(e.txn_id, younger);
    }
    FLINTDB_CHECK(threw);
}
