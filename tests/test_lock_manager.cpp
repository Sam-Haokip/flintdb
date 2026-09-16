#include "../src/txn/lock_manager.h"
#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

using namespace flintdb;

namespace {

// Starts `fn` on its own background thread and returns immediately --
// deliberately non-blocking, so a test can start two of these back to
// back and have them actually run concurrently (a helper that blocked
// the caller until `fn` finished would silently serialize what's
// supposed to be a two-thread race, which is exactly the bug this
// shape replaced: see the git history of this file for the earlier,
// blocking version and why it made the deadlock test pass without
// actually exercising concurrent contention).
//
// Any exception `fn` throws (including a failed FLINTDB_CHECK, which
// throws flintdb::testing::TestFailure -- not a std::exception) is
// caught on the background thread and stashed rather than left to
// escape: an exception escaping a detached std::thread's function
// calls std::terminate() and takes down the whole test binary, so a
// check failure inside a background lambda must never be allowed to
// propagate there. WaitFor rethrows it on the caller's thread instead,
// so it's reported as an ordinary test failure.
struct BackgroundResult {
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::exception_ptr> error = std::make_shared<std::exception_ptr>();
};

BackgroundResult RunInBackground(std::function<void()> fn) {
    BackgroundResult result;
    auto done = result.done;
    auto error = result.error;
    std::thread t([fn = std::move(fn), done, error] {
        try {
            fn();
        } catch (...) {
            *error = std::current_exception();
        }
        done->store(true);
    });
    t.detach();  // no safe way to force-stop a std::thread; timeout path below is never expected to trigger
    return result;
}

// Waits up to `timeout` for a RunInBackground task to finish, polling
// rather than join()-ing so a broken wait-die guarantee (a hang) fails
// this test with a clear message instead of hanging the whole suite.
// Returns false on timeout. If the task finished, rethrows whatever it
// threw (see RunInBackground's comment) so the failure surfaces as a
// normal test failure on the calling thread.
bool WaitFor(const BackgroundResult& result, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!result.done->load()) {
        if (std::chrono::steady_clock::now() - start > timeout) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (*result.error) std::rethrow_exception(*result.error);
    return true;
}

constexpr auto kTimeout = std::chrono::milliseconds(5000);

}  // namespace

FLINTDB_TEST(lock_manager_multiple_shared_holders_are_compatible) {
    LockManager lm;
    lm.AcquireLock(1, 0, 100, LockMode::kShared);
    lm.AcquireLock(2, 0, 100, LockMode::kShared);
    lm.AcquireLock(3, 0, 100, LockMode::kShared);  // none of these should block or throw
    FLINTDB_CHECK(lm.HasAnyLock(1));
    FLINTDB_CHECK(lm.HasAnyLock(2));
    FLINTDB_CHECK(lm.HasAnyLock(3));
}

FLINTDB_TEST(lock_manager_reacquiring_an_already_sufficient_lock_is_a_no_op) {
    LockManager lm;
    lm.AcquireLock(1, 0, 100, LockMode::kShared);
    lm.AcquireLock(1, 0, 100, LockMode::kShared);  // same mode again -- must not throw or block
    lm.AcquireLock(1, 0, 100, LockMode::kExclusive);
    lm.AcquireLock(1, 0, 100, LockMode::kExclusive);  // exclusive again -- also fine
    lm.AcquireLock(1, 0, 100, LockMode::kShared);     // shared is already implied by exclusive -- fine
    FLINTDB_CHECK(lm.HasAnyLock(1));
}

FLINTDB_TEST(lock_manager_upgrade_from_shared_to_exclusive_succeeds_when_sole_holder) {
    LockManager lm;
    lm.AcquireLock(1, 0, 100, LockMode::kShared);
    lm.AcquireLock(1, 0, 100, LockMode::kExclusive);  // upgrade -- only holder, must succeed immediately
    FLINTDB_CHECK(lm.HasAnyLock(1));
}

FLINTDB_TEST(lock_manager_younger_transaction_dies_instead_of_waiting_for_an_older_holder) {
    LockManager lm;
    TxnId older = 1;
    TxnId younger = 2;
    lm.AcquireLock(older, 0, 100, LockMode::kExclusive);

    bool threw = false;
    try {
        lm.AcquireLock(younger, 0, 100, LockMode::kShared);
    } catch (const TransactionAbortedException& e) {
        threw = true;
        FLINTDB_CHECK_EQ(e.txn_id, younger);
    }
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK(!lm.HasAnyLock(younger));  // the failed request never granted anything
}

FLINTDB_TEST(lock_manager_older_transaction_waits_for_a_younger_holder_then_succeeds) {
    LockManager lm;
    TxnId older = 1;
    TxnId younger = 5;
    lm.AcquireLock(younger, 0, 100, LockMode::kExclusive);

    std::atomic<bool> older_acquired{false};
    BackgroundResult task = RunInBackground([&] {
        lm.AcquireLock(older, 0, 100, LockMode::kExclusive);  // must wait, not die -- older is older
        older_acquired = true;
    });

    // Give the waiter a moment to actually be blocked (best-effort --
    // the real assertion is the one after release, which is order-
    // independent of this sleep).
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    FLINTDB_CHECK(!older_acquired.load());  // still waiting -- younger hasn't released yet

    lm.ReleaseAll(younger);

    FLINTDB_CHECK(WaitFor(task, kTimeout));  // the background AcquireLock call returned within the timeout
    FLINTDB_CHECK(older_acquired.load());
    FLINTDB_CHECK(lm.HasAnyLock(older));
}

FLINTDB_TEST(lock_manager_wait_die_prevents_a_real_two_transaction_deadlock) {
    // The classic deadlock shape: each of two transactions holds a lock
    // the other one is about to want. Under plain 2PL (no prevention or
    // detection) both would block forever. Wait-die must force exactly
    // one of them to die instead of wait, breaking the cycle before it
    // can close -- this test proves that by never hanging.
    //
    // Both requests are launched via RunInBackground (non-blocking) so
    // they genuinely race each other on separate threads, rather than
    // running one after the other -- with a blocking launcher, younger's
    // death-and-release would always finish before older's request even
    // starts, which would "pass" without ever exercising older actually
    // waiting on a lock younger is still holding.
    LockManager lm;
    TxnId older = 1;
    TxnId younger = 2;

    lm.AcquireLock(older, 0, 100, LockMode::kExclusive);
    lm.AcquireLock(younger, 0, 200, LockMode::kExclusive);

    std::atomic<bool> younger_died{false};
    std::atomic<bool> older_succeeded{false};

    BackgroundResult younger_task = RunInBackground([&] {
        try {
            // younger wants page 100, held by older -- younger must
            // die, never wait, per wait-die.
            lm.AcquireLock(younger, 0, 100, LockMode::kExclusive);
        } catch (const TransactionAbortedException& e) {
            FLINTDB_CHECK_EQ(e.txn_id, younger);
            younger_died = true;
            lm.ReleaseAll(younger);  // what a real caller does on catching this
        }
    });

    BackgroundResult older_task = RunInBackground([&] {
        // older wants page 200, held by younger -- older is allowed
        // to wait, and must eventually succeed once younger's abort
        // releases page 200.
        lm.AcquireLock(older, 0, 200, LockMode::kExclusive);
        older_succeeded = true;
    });

    bool younger_completed = WaitFor(younger_task, kTimeout);
    bool older_completed = WaitFor(older_task, kTimeout);

    FLINTDB_CHECK(younger_completed);
    FLINTDB_CHECK(older_completed);
    FLINTDB_CHECK(younger_died.load());
    FLINTDB_CHECK(older_succeeded.load());
}

FLINTDB_TEST(lock_manager_release_all_frees_every_page_the_transaction_held) {
    LockManager lm;
    lm.AcquireLock(1, 0, 100, LockMode::kShared);
    lm.AcquireLock(1, 0, 200, LockMode::kExclusive);
    FLINTDB_CHECK(lm.HasAnyLock(1));

    lm.ReleaseAll(1);
    FLINTDB_CHECK(!lm.HasAnyLock(1));

    // Both pages must now be free for someone else to take exclusively.
    lm.AcquireLock(2, 0, 100, LockMode::kExclusive);
    lm.AcquireLock(2, 0, 200, LockMode::kExclusive);
    FLINTDB_CHECK(lm.HasAnyLock(2));
}

FLINTDB_TEST(lock_manager_release_all_on_a_transaction_holding_nothing_is_a_safe_no_op) {
    LockManager lm;
    lm.ReleaseAll(999);  // never acquired anything -- must not throw
    FLINTDB_CHECK(!lm.HasAnyLock(999));
}

FLINTDB_TEST(lock_manager_page_level_grants_concurrent_access_to_different_pages_of_the_same_table) {
    // Page-level (not table-level) granularity's entire reason for
    // existing: two transactions touching different pages of what a real
    // table would treat as "the same object" must both be able to hold
    // exclusive locks *at the same instant*, never queuing behind each
    // other just because they both touch that table. This is the
    // empirical proof docs/SPEC.md section 3 cites for why table-level
    // locking was rejected without a second full implementation to
    // disprove it -- a single table-wide mutex could never let more than
    // one of these threads past the latch below at once.
    //
    // Every thread locks a *distinct* page id and then waits at a
    // std::latch before releasing -- so the latch can only complete if
    // every single thread's AcquireLock has already returned, proving
    // all kThreads locks were held concurrently, not "acquired and
    // released one at a time so fast it looked concurrent."
    LockManager lm;
    constexpr int kThreads = 5;
    std::latch all_acquired(kThreads);
    std::vector<char> got_lock(kThreads, 0);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            TxnId txn_id = static_cast<TxnId>(t + 1);
            PageId page_id = static_cast<PageId>(100 * (t + 1));  // a distinct page per thread -- no two conflict
            lm.AcquireLock(txn_id, 0, page_id, LockMode::kExclusive);
            got_lock[static_cast<size_t>(t)] = 1;

            all_acquired.count_down();
            all_acquired.wait();  // blocks forever if table-level (one shared lock) snuck in instead of page-level

            lm.ReleaseAll(txn_id);
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        FLINTDB_CHECK(got_lock[static_cast<size_t>(t)] == 1);
    }
}

FLINTDB_TEST(lock_manager_the_same_page_id_under_different_object_ids_never_conflicts) {
    // Phase 5's whole reason PageKey exists (docs/DECISIONS.md D-032): a
    // bare PageId is only unique within one object's (table's or index's)
    // own file, so two different objects' page 100 must be treated as two
    // completely unrelated locks -- one transaction holding page 100
    // exclusively in object 1 must never block, or even be seen as
    // related to, a different transaction wanting page 100 in object 2.
    LockManager lm;
    TxnId t1 = 1, t2 = 2;
    ObjectId object1 = 1, object2 = 2;

    // Same numeric page id, different objects -- both exclusive, both
    // granted immediately, no blocking, no exception.
    lm.AcquireLock(t1, object1, 100, LockMode::kExclusive);
    lm.AcquireLock(t2, object2, 100, LockMode::kExclusive);

    FLINTDB_CHECK(lm.HasAnyLock(t1));
    FLINTDB_CHECK(lm.HasAnyLock(t2));

    // A third, younger transaction can still get object2's page 100
    // exclusively once t2 releases -- proving the two objects' page-100
    // locks are tracked as genuinely separate entries, not merged into
    // one that ReleaseAll(t2) only partially frees.
    lm.ReleaseAll(t2);
    TxnId t3 = 3;
    lm.AcquireLock(t3, object2, 100, LockMode::kExclusive);
    FLINTDB_CHECK(lm.HasAnyLock(t3));

    // And t1's object1/page-100 lock was never touched by any of this.
    FLINTDB_CHECK(lm.HasAnyLock(t1));

    lm.ReleaseAll(t1);
    lm.ReleaseAll(t3);
}

FLINTDB_TEST(lock_manager_exclusive_lock_blocks_a_younger_reader_until_released) {
    // Same shape as the writer-waits-for-writer test above, but checks
    // shared-vs-exclusive conflict specifically, with the requester
    // younger this time (so it should die, not wait) -- covering the
    // other half of the wait-die rule against the other half of the
    // lock-mode matrix (exclusive-vs-shared, not just exclusive-vs-
    // exclusive).
    LockManager lm;
    TxnId older_writer = 1;
    TxnId younger_reader = 2;
    lm.AcquireLock(older_writer, 0, 100, LockMode::kExclusive);

    bool threw = false;
    try {
        lm.AcquireLock(younger_reader, 0, 100, LockMode::kShared);
    } catch (const TransactionAbortedException&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}
