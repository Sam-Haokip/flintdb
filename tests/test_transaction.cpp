#include "../src/storage/heap_file.h"
#include "../src/txn/transaction_manager.h"
#include "../src/wal/recovery.h"
#include "test_framework.h"
#include "test_utils.h"

#include <stdexcept>

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
