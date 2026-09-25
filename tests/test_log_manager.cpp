#include "../src/wal/log_manager.h"
#include "test_framework.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <set>
#include <thread>
#include <vector>

using namespace flintdb;
using flintdb::testing::CorruptByteAt;
using flintdb::testing::FileSizeOf;
using flintdb::testing::TempFile;
using flintdb::testing::TruncateFileTo;

namespace {

std::array<char, PAGE_SIZE> MakePageImage(char fill) {
    std::array<char, PAGE_SIZE> img;
    img.fill(fill);
    return img;
}

// Runs `fn` on its own detached thread with a generous-but-finite budget,
// the same pattern tests/test_storage_corruption.cpp uses for D-054's
// bounded-traversal fix: FlushThrough (docs/DECISIONS.md D-055) is new,
// nontrivial cross-thread synchronization (a condition-variable loop), so
// every test exercising it here fails loudly on a timeout instead of
// hanging the whole test binary if a lost-wakeup or similar bug is ever
// introduced. Returns true if `fn` returned within `budget` (whether or
// not it threw); `*thrown` is set to whatever it threw, if anything. The
// worker thread is deliberately detached, not joined, so a genuine hang
// here can't also hang this test binary.
bool RunBounded(std::function<void()> fn, std::exception_ptr* thrown,
                 std::chrono::seconds budget = std::chrono::seconds(10)) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> done_future = done->get_future();
    std::thread worker([fn = std::move(fn), done, thrown]() {
        try {
            fn();
        } catch (...) {
            *thrown = std::current_exception();
        }
        done->set_value();
    });
    worker.detach();
    return done_future.wait_for(budget) == std::future_status::ready;
}

}  // namespace

FLINTDB_TEST(log_manager_fresh_file_starts_with_no_records_and_lsn_one) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    FLINTDB_CHECK(log.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(log.NextLsn(), 1u);
}

FLINTDB_TEST(log_manager_append_assigns_strictly_increasing_lsns) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    Lsn l1 = log.AppendBegin(1);
    Lsn l2 = log.AppendCommit(1);
    Lsn l3 = log.AppendBegin(2);
    FLINTDB_CHECK_EQ(l1, 1u);
    FLINTDB_CHECK_EQ(l2, 2u);
    FLINTDB_CHECK_EQ(l3, 3u);
    FLINTDB_CHECK_EQ(log.NextLsn(), 4u);
}

FLINTDB_TEST(log_manager_reopen_sees_every_appended_record_in_order_with_correct_fields) {
    TempFile tmp("wal");
    auto image = MakePageImage('Q');
    {
        LogManager log(tmp.path());
        log.AppendBegin(42);
        log.AppendUpdate(42, 5, 7, image.data());
        log.AppendCommit(42);
        log.Flush();
    }

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 3u);

    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(records[0].lsn, 1u);
    FLINTDB_CHECK_EQ(records[0].txn_id, 42u);

    FLINTDB_CHECK(records[1].type == LogRecordType::kUpdate);
    FLINTDB_CHECK_EQ(records[1].lsn, 2u);
    FLINTDB_CHECK_EQ(records[1].txn_id, 42u);
    FLINTDB_CHECK_EQ(records[1].object_id, 5u);
    FLINTDB_CHECK_EQ(records[1].page_id, 7u);
    FLINTDB_CHECK(records[1].page_image == image);

    FLINTDB_CHECK(records[2].type == LogRecordType::kCommit);
    FLINTDB_CHECK_EQ(records[2].lsn, 3u);
    FLINTDB_CHECK_EQ(records[2].txn_id, 42u);

    FLINTDB_CHECK_EQ(reopened.NextLsn(), 4u);
}

FLINTDB_TEST(log_manager_abort_record_round_trips) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        log.AppendAbort(1);
    }
    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK(records[1].type == LogRecordType::kAbort);
    FLINTDB_CHECK_EQ(records[1].txn_id, 1u);
}

FLINTDB_TEST(log_manager_multiple_update_records_each_keep_their_own_page_image) {
    TempFile tmp("wal");
    auto image_a = MakePageImage('A');
    auto image_b = MakePageImage('B');
    {
        LogManager log(tmp.path());
        log.AppendUpdate(1, 0, 10, image_a.data());
        log.AppendUpdate(1, 0, 20, image_b.data());
    }
    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK_EQ(records[0].page_id, 10u);
    FLINTDB_CHECK(records[0].page_image == image_a);
    FLINTDB_CHECK_EQ(records[1].page_id, 20u);
    FLINTDB_CHECK(records[1].page_image == image_b);
}

FLINTDB_TEST(log_manager_update_records_keep_object_id_independent_of_page_id) {
    // Phase 5, docs/DECISIONS.md D-032: the WAL's whole reason object_id
    // exists is that page_id alone is only unique within one object's own
    // file. Two Update records with the *same* page_id but different
    // object_id must round-trip as two distinct, independently-addressed
    // records, not collapse or get confused with each other.
    TempFile tmp("wal");
    auto image_a = MakePageImage('A');
    auto image_b = MakePageImage('B');
    {
        LogManager log(tmp.path());
        log.AppendUpdate(1, 3, 100, image_a.data());  // object 3, page 100
        log.AppendUpdate(1, 9, 100, image_b.data());  // object 9, *same* page 100
    }
    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK_EQ(records[0].object_id, 3u);
    FLINTDB_CHECK_EQ(records[0].page_id, 100u);
    FLINTDB_CHECK(records[0].page_image == image_a);
    FLINTDB_CHECK_EQ(records[1].object_id, 9u);
    FLINTDB_CHECK_EQ(records[1].page_id, 100u);
    FLINTDB_CHECK(records[1].page_image == image_b);
}

FLINTDB_TEST(log_manager_torn_tail_after_a_full_record_is_discarded_and_file_is_truncated) {
    TempFile tmp("wal");
    size_t good_size;
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        good_size = FileSizeOf(tmp.path());
        // Simulate a crash mid-write of the *next* record: an Update
        // record (large payload) that never finished landing on disk.
        auto image = MakePageImage('Z');
        log.AppendUpdate(1, 0, 3, image.data());
    }
    size_t full_size = FileSizeOf(tmp.path());
    FLINTDB_CHECK(full_size > good_size);
    // Cut the file off partway through the second record's payload --
    // exactly what a crash mid-pwrite leaves behind.
    TruncateFileTo(tmp.path(), good_size + 40);

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 1u);  // only the Begin record survives
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 2u);

    // The file on disk must actually have been truncated back to the
    // last good record -- not just skipped over in memory -- so a later
    // Append* lands right after it instead of after the garbage.
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), good_size);
}

FLINTDB_TEST(log_manager_torn_write_of_the_very_first_record_leaves_an_empty_log) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        auto image = MakePageImage('Z');
        log.AppendUpdate(1, 0, 3, image.data());
    }
    // Truncate mid-header of the one and only record.
    TruncateFileTo(tmp.path(), 10);

    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 1u);
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
}

FLINTDB_TEST(log_manager_corrupted_checksum_on_a_full_record_is_treated_as_a_torn_tail) {
    TempFile tmp("wal");
    size_t good_size;
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        good_size = FileSizeOf(tmp.path());
        log.AppendCommit(1);
    }
    // Flip a byte inside the second record's payload region (its txn_id
    // field) without changing its length -- a bit-flip, not a truncation.
    // The record is structurally complete but its checksum no longer
    // matches, which must be treated exactly like a torn write: stop
    // there, keep everything before it.
    CorruptByteAt(tmp.path(), good_size + 9 /* kOffTxnId */);

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 1u);
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), good_size);
}

FLINTDB_TEST(log_manager_corrupted_page_image_byte_is_detected_via_checksum) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        auto image = MakePageImage('M');
        log.AppendUpdate(1, 0, 5, image.data());
    }
    // Flip a byte deep inside the update record's page-image payload
    // (well past the 29-byte header).
    CorruptByteAt(tmp.path(), 29 + 2000);

    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
}

FLINTDB_TEST(log_manager_checkpoint_wipes_the_file_and_resets_lsn) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    log.AppendBegin(1);
    log.AppendCommit(1);
    FLINTDB_CHECK(FileSizeOf(tmp.path()) > 0);

    log.Checkpoint();

    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
    FLINTDB_CHECK_EQ(log.NextLsn(), 1u);

    // A record appended right after checkpoint gets LSN 1 again, and a
    // fresh reopen sees only it.
    Lsn lsn = log.AppendBegin(99);
    FLINTDB_CHECK_EQ(lsn, 1u);
}

FLINTDB_TEST(log_manager_reopen_after_checkpoint_sees_an_empty_log) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        log.AppendCommit(1);
        log.Checkpoint();
    }
    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 1u);
}

FLINTDB_TEST(log_manager_flush_does_not_throw_and_records_remain_readable) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    log.AppendBegin(1);
    log.Flush();
    log.AppendCommit(1);
    log.Flush();
    FLINTDB_CHECK_EQ(log.RecordsOnOpen().size(), 0u);  // RecordsOnOpen is fixed at construction time
}

FLINTDB_TEST(log_manager_concurrent_appends_assign_unique_lsns_that_never_land_out_of_order_on_disk) {
    // Phase 4: many threads appending at once must never (a) hand out a
    // duplicate LSN, or (b) let a later-LSN record's bytes land in the
    // file before an earlier-LSN record's bytes -- the second is the
    // subtler bug: each individual write() is atomic (O_APPEND), but
    // without AppendRecord's own mutex serializing "assign LSN, then
    // write" as one unit, two threads could still assign LSNs 5 and 6
    // and then have LSN 6's write() physically complete first. Since the
    // constructor's parser never itself checks LSN monotonicity (it only
    // checks type/payload/checksum -- see the class comment), a genuine
    // out-of-order write would silently corrupt recovery's LSN-order
    // replay rather than fail loudly here, so this test checks file
    // order against LSN order directly, via a fresh reopen.
    TempFile tmp("wal");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    std::vector<std::vector<Lsn>> results(kThreads);
    {
        LogManager log(tmp.path());
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, &results, t] {
                results[t].reserve(kPerThread);
                for (int i = 0; i < kPerThread; ++i) {
                    results[t].push_back(log.AppendBegin(static_cast<TxnId>(t * kPerThread + i)));
                }
            });
        }
        for (auto& th : threads) th.join();
    }  // LogManager destructs here, closing the fd -- reopen fresh below to read purely from disk

    std::set<Lsn> all_lsns;
    for (auto& per_thread : results) {
        for (Lsn lsn : per_thread) all_lsns.insert(lsn);
    }
    FLINTDB_CHECK_EQ(all_lsns.size(), static_cast<size_t>(kThreads * kPerThread));  // no duplicates
    FLINTDB_CHECK_EQ(*all_lsns.begin(), 1u);
    FLINTDB_CHECK_EQ(*all_lsns.rbegin(), static_cast<Lsn>(kThreads * kPerThread));

    LogManager reopened(tmp.path());
    const auto& on_disk = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(on_disk.size(), static_cast<size_t>(kThreads * kPerThread));
    for (size_t i = 0; i + 1 < on_disk.size(); ++i) {
        FLINTDB_CHECK(on_disk[i].lsn < on_disk[i + 1].lsn);  // strictly increasing -- file order == LSN order
    }
}

// --- FlushThrough / group commit (Phase 7, docs/DECISIONS.md D-055) ---
//
// The performance benefit (fewer fsync calls under concurrent commits) is
// what docs/BENCHMARK_PHASE7.md measures; these three tests check
// FlushThrough's correctness instead: it actually makes the requested LSN
// durable, an already-covered target returns without hanging, and many
// threads calling it concurrently all complete with none left stranded --
// the exact property a lost-wakeup bug in its condition-variable loop
// would break. Every test here runs the operation under test through
// RunBounded (this file's anonymous namespace) rather than a plain call,
// so a regression fails loudly on a timeout instead of hanging the whole
// suite.

FLINTDB_TEST(log_manager_flush_through_makes_the_target_lsn_durable) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    log.AppendBegin(1);
    Lsn commit_lsn = log.AppendCommit(1);
    FLINTDB_CHECK_EQ(log.DurableLsn(), 0u);  // nothing flushed yet

    std::exception_ptr thrown;
    bool completed = RunBounded([&] { log.FlushThrough(commit_lsn); }, &thrown);
    FLINTDB_CHECK(completed);  // must not hang -- D-055's coalescing loop is new cross-thread synchronization
    if (completed) {
        FLINTDB_CHECK(thrown == nullptr);
        FLINTDB_CHECK(log.DurableLsn() >= commit_lsn);
    }
}

FLINTDB_TEST(log_manager_flush_through_with_an_already_covered_target_returns_without_hanging) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    Lsn begin_lsn = log.AppendBegin(1);
    Lsn commit_lsn = log.AppendCommit(1);

    std::exception_ptr thrown1;
    FLINTDB_CHECK(RunBounded([&] { log.FlushThrough(commit_lsn); }, &thrown1));
    FLINTDB_CHECK(thrown1 == nullptr);
    FLINTDB_CHECK(log.DurableLsn() >= commit_lsn);

    // begin_lsn < commit_lsn is already durable from the flush above, and
    // nothing new has been appended since -- this call has no record left
    // to wait for, so it must return immediately rather than block
    // waiting for a flush that will never come.
    std::exception_ptr thrown2;
    bool completed2 = RunBounded([&] { log.FlushThrough(begin_lsn); }, &thrown2);
    FLINTDB_CHECK(completed2);
    if (completed2) FLINTDB_CHECK(thrown2 == nullptr);
}

FLINTDB_TEST(log_manager_flush_through_many_concurrent_commits_all_become_durable_and_none_hang) {
    // The coalescing benefit itself is measured in docs/BENCHMARK_PHASE7.md
    // (fewer fsyncs under concurrent commits, translating to higher
    // throughput) -- not re-proven here. What this checks is correctness
    // under the exact load pattern that benefit depends on: every
    // thread's own commit record must eventually become durable, no
    // thread's FlushThrough call may throw, and nothing may hang.
    TempFile tmp("wal");
    LogManager log(tmp.path());
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;

    std::atomic<int> exceptions{0};
    auto work = [&log, &exceptions] {
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&log, &exceptions, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    try {
                        Lsn lsn = log.AppendCommit(static_cast<TxnId>(t * kPerThread + i));
                        log.FlushThrough(lsn);
                    } catch (...) {
                        exceptions.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    };

    std::exception_ptr thrown;
    bool completed = RunBounded(work, &thrown, std::chrono::seconds(30));
    FLINTDB_CHECK(completed);  // must not hang under real concurrent load
    if (completed) {
        FLINTDB_CHECK(thrown == nullptr);
        FLINTDB_CHECK_EQ(exceptions.load(), 0);
        FLINTDB_CHECK_EQ(log.DurableLsn(), log.NextLsn() - 1);  // every appended record ended up durable
    }
}
