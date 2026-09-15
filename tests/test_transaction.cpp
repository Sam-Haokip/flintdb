#include "../src/storage/heap_file.h"
#include "../src/txn/transaction_manager.h"
#include "../src/wal/recovery.h"
#include "test_framework.h"
#include "test_utils.h"

#include <atomic>
#include <latch>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace flintdb;
using flintdb::testing::TempFile;

FLINTDB_TEST(transaction_begin_returns_active_transaction_with_increasing_ids) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    FLINTDB_CHECK(!txm.HasActiveTransaction());
    Transaction* t1 = txm.Begin();
    FLINTDB_CHECK(t1 != nullptr);
    FLINTDB_CHECK(txm.HasActiveTransaction());
    TxnId id1 = t1->Id();
    txm.Commit(t1);

    Transaction* t2 = txm.Begin();
    FLINTDB_CHECK(t2->Id() != id1);
    FLINTDB_CHECK(t2->Id() > id1);
    txm.Commit(t2);
}

FLINTDB_TEST(transaction_begin_while_one_is_active_throws) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    txm.Begin();
    bool threw = false;
    try {
        txm.Begin();
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(transaction_commit_of_a_non_active_transaction_throws) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* t1 = txm.Begin();
    txm.Commit(t1);  // t1 is no longer active

    bool threw = false;
    try {
        txm.Commit(t1);
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(transaction_abort_of_a_non_active_transaction_throws) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* t1 = txm.Begin();
    txm.Abort(t1);

    bool threw = false;
    try {
        txm.Abort(t1);
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(transaction_commit_ends_the_active_transaction) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* t1 = txm.Begin();
    txm.Commit(t1);
    FLINTDB_CHECK(!txm.HasActiveTransaction());
}

FLINTDB_TEST(transaction_abort_ends_the_active_transaction) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* t1 = txm.Begin();
    txm.Abort(t1);
    FLINTDB_CHECK(!txm.HasActiveTransaction());
}

FLINTDB_TEST(transaction_commit_logs_update_records_then_a_commit_record_and_flushes) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);
    HeapFile heap(&bp);

    Transaction* txn = txm.Begin();
    heap.Insert("row-a");
    heap.Insert("row-b");
    txm.Commit(txn);

    // Reopen the WAL fresh to see exactly what was durably written.
    LogManager reopened(wal_tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK(records.size() >= 2u);  // at least one Begin, at least one Commit
    FLINTDB_CHECK(records.front().type == LogRecordType::kBegin);
    FLINTDB_CHECK(records.back().type == LogRecordType::kCommit);
    // Every record in between (if any) must be an Update belonging to
    // the same transaction as the Begin/Commit that bracket it.
    for (size_t i = 1; i + 1 < records.size(); i++) {
        FLINTDB_CHECK(records[i].type == LogRecordType::kUpdate);
        FLINTDB_CHECK_EQ(records[i].txn_id, records.front().txn_id);
    }
}

FLINTDB_TEST(transaction_commit_with_no_dirtied_pages_logs_only_begin_and_commit) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* txn = txm.Begin();
    txm.Commit(txn);  // nothing touched in between

    LogManager reopened(wal_tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK(records[1].type == LogRecordType::kCommit);
}

FLINTDB_TEST(transaction_abort_logs_no_update_records_only_begin_and_abort) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);
    HeapFile heap(&bp);

    Transaction* txn = txm.Begin();
    heap.Insert("will-be-aborted");
    txm.Abort(txn);

    LogManager reopened(wal_tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK(records[1].type == LogRecordType::kAbort);
}

FLINTDB_TEST(transaction_abort_reverts_a_heap_file_insert_so_the_row_is_gone) {
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);
    HeapFile heap(&bp);

    Transaction* txn = txm.Begin();
    heap.Insert("row-that-will-not-survive");
    FLINTDB_CHECK_EQ(heap.NumRows(), 1u);

    txm.Abort(txn);

    // The in-memory page was reverted out from under HeapFile -- a fresh
    // scan must see nothing (the row's slot never existed as far as the
    // on-disk/reverted content is concerned).
    FLINTDB_CHECK_EQ(heap.NumRows(), 0u);
}

FLINTDB_TEST(transaction_abort_after_a_prior_committed_insert_only_reverts_the_new_change) {
    // Regression test for a real bug caught during design, not a
    // hypothetical: Abort reverts pages by re-reading them from the data
    // file (BufferPool::DiscardPage), so if Commit did NOT force-flush,
    // t1's commit below would exist only in the WAL when t2 aborts, and
    // DiscardPage would reload the pre-t1 (stale) content -- silently
    // erasing t1's already-committed row along with t2's. Deliberately no
    // manual bp.FlushAll() anywhere in this test: Commit's own internal
    // force-flush (see transaction_manager.h's class comment) is what
    // must make this safe.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);
    HeapFile heap(&bp);

    Transaction* t1 = txm.Begin();
    heap.Insert("committed-row");
    txm.Commit(t1);

    Transaction* t2 = txm.Begin();
    heap.Insert("row-that-will-not-survive");
    FLINTDB_CHECK_EQ(heap.NumRows(), 2u);
    txm.Abort(t2);

    FLINTDB_CHECK_EQ(heap.NumRows(), 1u);
    FLINTDB_CHECK_EQ(heap.Scan()[0].second, std::string("committed-row"));
}

FLINTDB_TEST(transaction_commit_force_flushes_so_the_data_file_is_immediately_consistent) {
    // Directly checks the "force" half of Commit's no-steal-plus-force
    // policy: read the data file back through a brand-new BufferPool
    // (same DiskManager, so this sees exactly what's on disk) with no
    // recovery step involved at all -- if Commit didn't flush, this page
    // would still be all-zero.
    TempFile db_tmp;
    DiskManager dm(db_tmp.path());
    PageId written_pid;
    {
        BufferPool bp(&dm);
        TempFile wal_tmp("wal");
        LogManager log(wal_tmp.path());
        TransactionManager txm(&bp, &log);
        HeapFile heap(&bp);

        Transaction* txn = txm.Begin();
        RID rid = heap.Insert("flushed-by-commit");
        written_pid = rid.page_id;
        txm.Commit(txn);
    }

    BufferPool bp2(&dm);  // fresh cache, same underlying disk manager/file
    Page* reloaded = bp2.FetchPage(written_pid);
    FLINTDB_CHECK_EQ(reloaded->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(reloaded->GetRecord(0), std::string("flushed-by-commit"));
}

FLINTDB_TEST(transaction_commit_survives_a_restart_even_without_an_explicit_buffer_pool_flush) {
    // The caller-facing contract: after Commit() returns, the caller
    // never has to call BufferPool::FlushAll() themselves for the change
    // to survive a restart. Commit's own internal force-flush (see the
    // previous test) already guarantees the data file has it, and the
    // WAL still has the same Update/Commit records too (nothing clears
    // the WAL except an explicit Checkpoint(), which this test never
    // calls) -- so RunRecovery redoes the same bytes redundantly here,
    // which is exactly the idempotent behavior wal/recovery.h documents.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        TransactionManager txm(&bp, &log);
        HeapFile heap(&bp);

        Transaction* txn = txm.Begin();
        heap.Insert("durable-via-wal-only");
        txm.Commit(txn);
        // Deliberately no bp.FlushAll() here -- everything below must
        // come from the WAL, not from the data file bp wrote directly.
    }

    // "Restart": brand-new objects over the same on-disk files.
    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    size_t replayed = RunRecovery(log2.RecordsOnOpen(), &dm2);
    FLINTDB_CHECK(replayed >= 1u);

    BufferPool bp2(&dm2);
    HeapFile heap2(&bp2);
    auto rows = heap2.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 1u);
    FLINTDB_CHECK_EQ(rows[0].second, std::string("durable-via-wal-only"));
}

FLINTDB_TEST(transaction_never_committed_changes_do_not_survive_a_restart) {
    // The mirror image of the above: a transaction that began, dirtied a
    // page, and then the "process" ended (no Commit, no Abort -- exactly
    // what a real crash mid-transaction leaves behind, since there was no
    // chance to call either). Recovery must reconstruct nothing for it.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        TransactionManager txm(&bp, &log);
        HeapFile heap(&bp);

        txm.Begin();
        heap.Insert("never-committed");
        // No Commit(), no Abort() -- simulates a hard crash mid-transaction.
    }

    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    size_t replayed = RunRecovery(log2.RecordsOnOpen(), &dm2);
    FLINTDB_CHECK_EQ(replayed, 0u);

    BufferPool bp2(&dm2);
    HeapFile heap2(&bp2);
    FLINTDB_CHECK_EQ(heap2.NumRows(), 0u);
}

FLINTDB_TEST(transaction_manager_multiple_threads_each_have_their_own_active_transaction_simultaneously) {
    // Phase 4's actual headline feature: N threads, each running its own
    // transaction, genuinely overlapping in time -- not simulated
    // interleaving, and not serialized to "really" one-at-a-time under
    // the hood. Deliberately does NOT use HeapFile here: HeapFile's own
    // page_ids_ bookkeeping isn't lock-protected until Phase 4 wires
    // locking into its call sites (a separate, later task), so a shared
    // HeapFile across threads would race on *that*, which is not what
    // this test is checking. Each thread instead allocates and dirties
    // its own page directly through BufferPool (already proven
    // thread-safe on its own), keeping this test's only proof-of-concurrency
    // dependency on TransactionManager's own Begin/Commit/NumActiveTransactions.
    //
    // Two latches make the overlap deterministic rather than a race that
    // usually-but-not-always overlaps: every thread's Begin() must
    // complete before any thread checks NumActiveTransactions() (first
    // latch), and every thread's check must complete before any thread
    // commits (second latch) -- so the check is guaranteed to observe
    // all kThreads transactions active at once, not however many happened
    // to still be uncommitted by chance.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    constexpr int kThreads = 6;
    std::latch all_begun(kThreads);
    std::latch all_checked(kThreads);
    std::atomic<int> saw_full_overlap_count{0};
    // Plain char, not FLINTDB_CHECK inside the thread lambda: a failed
    // FLINTDB_CHECK throws flintdb::testing::TestFailure, and an
    // exception that escapes a std::thread's entry function (joined or
    // not) calls std::terminate() before join() gets a chance to do
    // anything -- crashing the whole test binary instead of reporting a
    // clean failure. Recording into this array and asserting after every
    // thread has joined (join() happens-before the read, so no separate
    // synchronization is needed) keeps a genuine failure here reportable.
    std::vector<char> insert_ok(kThreads, 0);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            Transaction* txn = txm.Begin();
            PageId pid;
            Page* page = bp.NewHeapPage(&pid);  // NewHeapPage inits the slotted layout AND marks it dirty
            auto slot = page->InsertRecord("owned-by-one-thread");
            insert_ok[static_cast<size_t>(t)] = slot.has_value() ? 1 : 0;

            all_begun.count_down();
            all_begun.wait();  // guaranteed: every thread's Begin() has now run

            if (txm.NumActiveTransactions() == static_cast<size_t>(kThreads)) {
                saw_full_overlap_count.fetch_add(1);
            }

            all_checked.count_down();
            all_checked.wait();  // guaranteed: every thread finished checking before any Commit()

            txm.Commit(txn);
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        FLINTDB_CHECK(insert_ok[static_cast<size_t>(t)] == 1);
    }
    // Every thread's own check must have seen the full overlap -- not
    // just "some thread got lucky."
    FLINTDB_CHECK_EQ(saw_full_overlap_count.load(), kThreads);
    FLINTDB_CHECK(!txm.HasActiveTransaction());
    FLINTDB_CHECK_EQ(txm.NumActiveTransactions(), 0u);
}

FLINTDB_TEST(transaction_manager_begin_is_per_thread_not_global) {
    // The per-thread nesting guard (Begin() throws if *this* thread
    // already has one) must not block a *different* thread from
    // beginning its own transaction at the same time -- that's the
    // actual distinction Phase 4 draws versus Phase 3's single global
    // restriction.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* main_txn = txm.Begin();  // main thread now has an active transaction

    bool other_thread_succeeded = false;
    std::thread other([&] {
        Transaction* other_txn = txm.Begin();  // a different thread -- must succeed even though main is active
        other_thread_succeeded = (other_txn != nullptr);
        txm.Commit(other_txn);
    });
    other.join();
    FLINTDB_CHECK(other_thread_succeeded);

    // Main thread's own nested Begin() must still throw.
    bool threw = false;
    try {
        txm.Begin();
    } catch (const std::logic_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    txm.Commit(main_txn);  // main thread's original transaction is still perfectly valid
}

FLINTDB_TEST(transaction_manager_commit_from_a_different_thread_than_began_it_throws) {
    // Committing must happen on the same thread that began the
    // transaction -- see transaction_manager.h's class comment on why
    // (thread-local context and the per-thread observer registry are
    // only meaningful for the owning thread). A cross-thread attempt
    // must fail loudly (std::logic_error) rather than silently corrupt
    // the wrong thread's context.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    Transaction* txn = txm.Begin();  // begun on this (main) test thread

    bool threw = false;
    std::thread other([&] {
        try {
            txm.Commit(txn);  // wrong thread -- must throw, not succeed
        } catch (const std::logic_error&) {
            threw = true;
        }
    });
    other.join();
    FLINTDB_CHECK(threw);

    // The original thread must still be able to commit its own
    // transaction normally -- the failed cross-thread attempt must not
    // have corrupted anything.
    FLINTDB_CHECK(txm.HasActiveTransaction());
    txm.Commit(txn);
    FLINTDB_CHECK(!txm.HasActiveTransaction());
}

FLINTDB_TEST(transaction_manager_concurrent_commits_each_log_a_correctly_ordered_begin_update_commit_group) {
    // End-to-end proof that concurrent transactions each produce a
    // correct, individually-attributable WAL trail: every transaction's
    // own Begin/Update/Commit records must all share that transaction's
    // txn_id and appear with LSNs in the right relative order (Begin
    // before its Update(s) before its Commit), even though many
    // transactions' records are interleaved in the single shared WAL
    // file. Each thread again uses its own page (see the concurrency
    // test above for why, re: HeapFile not being lock-safe yet).
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&bp, &log);

    constexpr int kThreads = 8;
    std::vector<TxnId> txn_ids(kThreads, 0);
    std::vector<PageId> page_ids(kThreads, 0);
    std::vector<char> insert_ok(kThreads, 0);  // see the earlier concurrency test for why not FLINTDB_CHECK here

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            Transaction* txn = txm.Begin();
            txn_ids[t] = txn->Id();
            PageId pid;
            Page* page = bp.NewHeapPage(&pid);  // NewHeapPage, not NewPage -- must init the slotted layout
            auto slot = page->InsertRecord("thread-" + std::to_string(t));
            insert_ok[static_cast<size_t>(t)] = slot.has_value() ? 1 : 0;
            page_ids[t] = pid;
            txm.Commit(txn);
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        FLINTDB_CHECK(insert_ok[static_cast<size_t>(t)] == 1);
    }

    std::set<TxnId> unique_ids(txn_ids.begin(), txn_ids.end());
    FLINTDB_CHECK_EQ(unique_ids.size(), static_cast<size_t>(kThreads));  // every thread got a distinct txn_id

    LogManager reopened(wal_tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), static_cast<size_t>(kThreads * 3));  // Begin + Update + Commit, each thread

    for (TxnId id : unique_ids) {
        // Find this transaction's own three records, in file (= LSN) order.
        std::vector<const LogRecord*> mine;
        for (const auto& r : records) {
            if (r.txn_id == id) mine.push_back(&r);
        }
        FLINTDB_CHECK_EQ(mine.size(), 3u);
        FLINTDB_CHECK(mine[0]->type == LogRecordType::kBegin);
        FLINTDB_CHECK(mine[1]->type == LogRecordType::kUpdate);
        FLINTDB_CHECK(mine[2]->type == LogRecordType::kCommit);
        FLINTDB_CHECK(mine[0]->lsn < mine[1]->lsn);
        FLINTDB_CHECK(mine[1]->lsn < mine[2]->lsn);
    }

    // And the data itself is durably correct for every thread's page.
    BufferPool bp2(&dm);
    for (int t = 0; t < kThreads; ++t) {
        Page* page = bp2.FetchPage(page_ids[t]);
        FLINTDB_CHECK_EQ(page->GetSlotCount(), 1u);
        FLINTDB_CHECK_EQ(page->GetRecord(0), std::string("thread-" + std::to_string(t)));
    }
}
