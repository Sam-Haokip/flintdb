// Full-stack crash-simulation tests, combining HeapFile, TransactionManager,
// LogManager, DiskManager and RunRecovery together -- the individual
// components each have their own focused tests (test_log_manager.cpp,
// test_recovery.cpp, test_transaction.cpp); this file is specifically
// about the properties docs/SPEC.md section 5 requires to be checked with
// an actual simulated crash (real files, manually truncated/corrupted
// bytes, then fresh objects reopened over the result) rather than by
// code inspection.

#include "../src/storage/heap_file.h"
#include "../src/txn/transaction_manager.h"
#include "../src/wal/recovery.h"
#include "test_framework.h"
#include "test_utils.h"

#include <set>

using namespace flintdb;
using flintdb::testing::FileSizeOf;
using flintdb::testing::TempFile;
using flintdb::testing::TruncateFileTo;

namespace {
std::multiset<std::string> RowContents(const std::vector<std::pair<RID, std::string>>& rows) {
    std::multiset<std::string> out;
    for (auto& [rid, bytes] : rows) out.insert(bytes);
    return out;
}
}  // namespace

FLINTDB_TEST(crash_mid_commit_record_write_leaves_the_transaction_uncommitted) {
    // The realistic version of "updates logged but no commit record":
    // Begin and Update are fully appended and fsynced (a real transaction
    // that got as far as deciding to commit), and the crash happens
    // exactly while the Commit record itself is being written -- so the
    // WAL file's tail is a torn, partial Commit record, not a missing one.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    PageId touched_pid;
    size_t size_after_update_fsync;
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        HeapFile heap(0, &bp);

        // Do exactly what TransactionManager::Commit does, but stop
        // right before (and during) the Commit record, so the crash can
        // be simulated precisely at that boundary.
        TxnId txn_id = 1;
        log.AppendBegin(txn_id);
        RID rid = heap.Insert("row-that-should-vanish");
        touched_pid = rid.page_id;
        Page* page = bp.FetchPage(touched_pid);
        log.AppendUpdate(txn_id, 0, touched_pid, page->Data());
        log.Flush();  // Begin + Update are durably on disk -- txn_id looks
                       // like it's about to commit, right up until this point
        size_after_update_fsync = FileSizeOf(wal_tmp.path());

        log.AppendCommit(txn_id);  // this write happens, but see below --
                                    // we're about to truncate it away to
                                    // simulate it never having completed
    }
    // The Commit record's on-disk size is small (kHeaderSize=25 +
    // payload 0 + checksum 4 = 29 bytes); cut it off partway through.
    TruncateFileTo(wal_tmp.path(), size_after_update_fsync + 10);

    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    const auto& records = log2.RecordsOnOpen();
    for (const auto& r : records) {
        FLINTDB_CHECK(r.type != LogRecordType::kCommit);  // the torn Commit never counts
    }

    size_t replayed = RunRecovery(records, {{0, &dm2}});
    FLINTDB_CHECK_EQ(replayed, 0u);

    BufferPool bp2(&dm2);
    HeapFile heap2(0, &bp2);
    FLINTDB_CHECK_EQ(heap2.NumRows(), 0u);
}

FLINTDB_TEST(checkpoint_then_crash_before_the_next_checkpoint_still_recovers_everything) {
    // Rows committed before a checkpoint (durable via the data file, since
    // checkpoint requires everything flushed first) and rows committed
    // after it (durable via the WAL, which only has post-checkpoint
    // records once Checkpoint() has wiped it) must both survive a crash
    // that happens with no further checkpoint in between.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        TransactionManager txm(&log);
        txm.RegisterObject(0, &bp);
        HeapFile heap(0, &bp);

        Transaction* t1 = txm.Begin();
        heap.Insert("before-checkpoint-1");
        heap.Insert("before-checkpoint-2");
        txm.Commit(t1);

        // Quiescent checkpoint: everything's already flushed (Commit force-
        // flushes -- see transaction_manager.h), so this is just wiping the
        // WAL and resetting its LSN counter.
        bp.FlushAll();
        log.Checkpoint();
        FLINTDB_CHECK_EQ(FileSizeOf(wal_tmp.path()), 0u);

        Transaction* t2 = txm.Begin();
        heap.Insert("after-checkpoint-1");
        txm.Commit(t2);
        // "Crash" here: no further checkpoint, no explicit final flush
        // beyond what Commit already did internally.
    }

    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    // The WAL only has the post-checkpoint transaction's records.
    for (const auto& r : log2.RecordsOnOpen()) {
        FLINTDB_CHECK(r.txn_id != 1u);  // txn 1 (pre-checkpoint) left no trace in the wiped log
    }
    RunRecovery(log2.RecordsOnOpen(), {{0, &dm2}});

    BufferPool bp2(&dm2);
    HeapFile heap2(0, &bp2);
    auto rows = heap2.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 3u);
    std::multiset<std::string> expected = {"before-checkpoint-1", "before-checkpoint-2", "after-checkpoint-1"};
    FLINTDB_CHECK(RowContents(rows) == expected);
}

FLINTDB_TEST(full_heap_file_transaction_crash_and_recover_integration) {
    // The end-to-end scenario: several transactions (some committed, one
    // aborted), a delete, and a genuine "crash" (objects just go out of
    // scope -- no clean shutdown, no final flush beyond what Commit does
    // internally) -- then a fresh process-equivalent reopens everything,
    // runs recovery, and must see exactly the rows that a correct
    // implementation would have kept.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        TransactionManager txm(&log);
        txm.RegisterObject(0, &bp);
        HeapFile heap(0, &bp);

        Transaction* t1 = txm.Begin();
        heap.Insert("alice");
        RID bob_rid = heap.Insert("bob");
        heap.Insert("carol");
        txm.Commit(t1);

        Transaction* t2 = txm.Begin();
        heap.Insert("row-that-will-be-aborted");
        txm.Abort(t2);

        Transaction* t3 = txm.Begin();
        FLINTDB_CHECK(heap.Delete(bob_rid));  // bob is gone
        heap.Insert("dave");
        txm.Commit(t3);

        txm.Begin();
        heap.Insert("never-committed-eve");
        // No Commit(), no Abort() -- simulates the crash happening with
        // t4 still open.
    }

    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    RunRecovery(log2.RecordsOnOpen(), {{0, &dm2}});

    BufferPool bp2(&dm2);
    HeapFile heap2(0, &bp2);
    auto rows = heap2.Scan();
    std::multiset<std::string> expected = {"alice", "carol", "dave"};
    FLINTDB_CHECK(RowContents(rows) == expected);
    FLINTDB_CHECK_EQ(heap2.NumRows(), 3u);
}

FLINTDB_TEST(recovery_is_idempotent_across_a_second_simulated_crash_during_recovery_itself) {
    // A crash can happen *during* recovery too (e.g. the process that's
    // replaying the WAL itself dies partway through). Since every Update
    // record is a whole-page-image redo, running recovery again from
    // scratch over the same (unmodified) WAL must reach the identical
    // final state -- this is what makes "just run recovery again" always
    // the right response to that scenario, with no special-casing needed.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    {
        DiskManager dm(db_tmp.path());
        BufferPool bp(&dm);
        LogManager log(wal_tmp.path());
        TransactionManager txm(&log);
        txm.RegisterObject(0, &bp);
        HeapFile heap(0, &bp);

        Transaction* t1 = txm.Begin();
        heap.Insert("row-1");
        heap.Insert("row-2");
        txm.Commit(t1);
    }

    DiskManager dm2(db_tmp.path());
    LogManager log2(wal_tmp.path());
    size_t first_replay = RunRecovery(log2.RecordsOnOpen(), {{0, &dm2}});
    // Simulate recovery itself being interrupted and restarted: just run
    // it again over the same records and DiskManager.
    size_t second_replay = RunRecovery(log2.RecordsOnOpen(), {{0, &dm2}});
    FLINTDB_CHECK_EQ(first_replay, second_replay);

    BufferPool bp2(&dm2);
    HeapFile heap2(0, &bp2);
    auto rows = heap2.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 2u);
    std::multiset<std::string> expected = {"row-1", "row-2"};
    FLINTDB_CHECK(RowContents(rows) == expected);
}
