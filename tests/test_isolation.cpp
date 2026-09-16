// Named isolation-anomaly tests, one per row of docs/SPEC.md section 3's
// table -- each proves (or, for the phantom-read row, precisely bounds)
// what Strict 2PL with page-level locking actually guarantees, by driving
// real HeapFile + TransactionManager traffic across real std::threads
// rather than asserting the guarantee by inspection (see docs/SPEC.md
// section 5's verification standard).
//
// A recurring, deliberate pattern below: which transaction is made the
// *older* one (begins first) is chosen by which side of the conflict the
// test needs to *wait* rather than *die*, per wait-die (lock_manager.h) --
// an older requester waits for a younger holder; a younger requester
// conflicting with an older holder dies immediately instead. That's
// sometimes the opposite of the scenario's narrative order (e.g. the
// "second transaction's write" begins before the "first transaction's
// read" in program order) -- each test says so explicitly where it
// matters.
#include "../src/storage/heap_file.h"
#include "../src/storage/page.h"
#include "../src/txn/transaction_manager.h"
#include "test_framework.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace flintdb;
using flintdb::testing::TempFile;

namespace {

// Same shape and rationale as tests/test_lock_manager.cpp's identically-named
// helpers (duplicated rather than shared, consistent with this project's
// existing per-file test-helper style): runs `fn` on a detached background
// thread so two sides of a conflict genuinely race instead of running
// serialized, and safely ferries any exception (including a failed
// FLINTDB_CHECK, which throws flintdb::testing::TestFailure -- not a
// std::exception, and would call std::terminate() if allowed to escape a
// std::thread's entry function) back to the calling thread via WaitFor.
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
    t.detach();
    return result;
}

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

void SpinUntil(const std::atomic<bool>& flag) {
    while (!flag.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

FLINTDB_TEST(isolation_dirty_read_is_prevented_the_reader_blocks_until_the_writer_commits) {
    // Classic dirty-read shape: one transaction writes a row but hasn't
    // committed yet; another transaction tries to read the same page.
    // This engine has no MVCC/versioning -- a reader either gets the page
    // as it currently sits (which would be the dirty, uncommitted value)
    // or it has to wait. Strict 2PL's exclusive lock, held by the writer
    // until commit, forces the latter: the reader physically cannot
    // proceed until the writer commits and releases it.
    //
    // The reader is deliberately begun *before* the writer (making it
    // the older transaction) so the conflict resolves as "older waits
    // for younger" rather than "younger dies" -- see the file comment.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&log);
    txm.RegisterObject(0, &bp);
    HeapFile heap(0, &bp);

    std::atomic<bool> reader_begun{false};
    std::atomic<bool> writer_inserted{false};
    std::atomic<bool> reader_returned{false};

    BackgroundResult reader_task = RunInBackground([&] {
        Transaction* reader_txn = txm.Begin();  // begins first -- older
        reader_begun = true;
        SpinUntil(writer_inserted);
        auto rows = heap.Scan();  // must block until the writer commits
        reader_returned = true;
        FLINTDB_CHECK_EQ(rows.size(), 1u);
        FLINTDB_CHECK_EQ(rows[0].second, std::string("uncommitted-row"));
        txm.Commit(reader_txn);
    });

    SpinUntil(reader_begun);

    BackgroundResult writer_task = RunInBackground([&] {
        Transaction* writer_txn = txm.Begin();  // begins second -- younger
        heap.Insert("uncommitted-row");
        writer_inserted = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // give the reader time to actually block
        FLINTDB_CHECK(!reader_returned.load());  // still blocked -- proves it never saw the dirty value
        txm.Commit(writer_txn);
    });

    FLINTDB_CHECK(WaitFor(writer_task, kTimeout));
    FLINTDB_CHECK(WaitFor(reader_task, kTimeout));
    FLINTDB_CHECK(reader_returned.load());
}

FLINTDB_TEST(isolation_lost_update_is_prevented_by_exclusive_locks_held_through_commit) {
    // Classic lost-update shape: two transactions each read a value,
    // compute value+1, and write it back. If both were allowed to read
    // the same pre-update value concurrently, whichever commits last
    // would silently clobber the other's increment -- final value 1, not
    // 2, with no error anywhere to notice it happened. Strict 2PL
    // prevents this because the second transaction to touch the row
    // can't even *read* it until the first one commits and releases its
    // exclusive lock, so it necessarily reads the post-commit value and
    // its own increment lands on top of, not instead of, the first.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&log);
    txm.RegisterObject(0, &bp);
    HeapFile heap(0, &bp);

    heap.Insert("counter:0");  // seed row, written with no transaction active

    auto increment = [&] {
        auto rows = heap.Scan();
        FLINTDB_CHECK_EQ(rows.size(), 1u);
        RID rid = rows[0].first;
        int value = std::stoi(rows[0].second.substr(rows[0].second.find(':') + 1));
        FLINTDB_CHECK(heap.Delete(rid));
        heap.Insert("counter:" + std::to_string(value + 1));
    };

    std::atomic<bool> a_begun{false};
    std::atomic<bool> b_locked{false};
    std::atomic<bool> a_returned{false};

    // A begins first -- older -- so it's the one that WAITS below rather
    // than dying, even though B is the transaction that runs its
    // increment first in real time.
    BackgroundResult a_task = RunInBackground([&] {
        Transaction* a_txn = txm.Begin();
        a_begun = true;
        SpinUntil(b_locked);
        increment();  // must block inside Scan() until B commits
        a_returned = true;
        txm.Commit(a_txn);
    });

    SpinUntil(a_begun);

    BackgroundResult b_task = RunInBackground([&] {
        Transaction* b_txn = txm.Begin();  // younger
        increment();  // counter:0 -> counter:1, and holds the page exclusively from here on
        b_locked = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        FLINTDB_CHECK(!a_returned.load());  // a is still blocked -- it cannot have read the stale value 0
        txm.Commit(b_txn);
    });

    FLINTDB_CHECK(WaitFor(b_task, kTimeout));
    FLINTDB_CHECK(WaitFor(a_task, kTimeout));

    auto final_rows = heap.Scan();
    FLINTDB_CHECK_EQ(final_rows.size(), 1u);
    FLINTDB_CHECK_EQ(final_rows[0].second, std::string("counter:2"));  // both increments landed -- neither was lost
}

FLINTDB_TEST(isolation_non_repeatable_read_is_prevented_because_the_writer_waits_out_the_whole_reader_transaction) {
    // A transaction that reads the same row twice must see the same
    // value both times, even if another transaction tries to update that
    // row in between. Strict 2PL gets this for free from the "strict"
    // part specifically: the reader's shared lock is held until *its
    // own* commit, not released after the first read the way plain 2PL's
    // growing/shrinking phases would technically still allow -- so a
    // concurrent writer cannot get in edgewise between the reader's two
    // reads at all, not even for an instant.
    //
    // The writer is deliberately begun *before* the reader (making it
    // the older transaction) specifically so it's the one that WAITS
    // when it tries to write, rather than dying outright -- which lets
    // this test observe it still blocked at the moment of the reader's
    // second read, then see it finally succeed only after the reader
    // fully commits. See the file comment for why the begin order here
    // is the opposite of the narrative order.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&log);
    txm.RegisterObject(0, &bp);
    HeapFile heap(0, &bp);

    RID seed_rid = heap.Insert("value:0");  // no transaction active yet

    std::atomic<bool> writer_begun{false};
    std::atomic<bool> reader_first_read_done{false};
    std::atomic<bool> writer_attempting{false};
    std::atomic<bool> writer_returned{false};

    BackgroundResult writer_task = RunInBackground([&] {
        Transaction* writer_txn = txm.Begin();  // begins first -- older
        writer_begun = true;
        SpinUntil(reader_first_read_done);
        writer_attempting = true;
        FLINTDB_CHECK(heap.Delete(seed_rid));  // must block -- reader (younger) still holds a shared lock on this page
        heap.Insert("value:1");
        writer_returned = true;
        txm.Commit(writer_txn);
    });

    SpinUntil(writer_begun);

    BackgroundResult reader_task = RunInBackground([&] {
        Transaction* reader_txn = txm.Begin();  // begins second -- younger
        auto first = heap.Scan();
        FLINTDB_CHECK_EQ(first.size(), 1u);
        std::string first_value = first[0].second;
        reader_first_read_done = true;

        SpinUntil(writer_attempting);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // give the writer time to actually block
        FLINTDB_CHECK(!writer_returned.load());  // writer is still blocked -- it hasn't touched the row yet

        auto second = heap.Scan();  // second read, same transaction
        FLINTDB_CHECK_EQ(second.size(), 1u);
        FLINTDB_CHECK_EQ(second[0].second, first_value);  // must be identical to the first read -- "value:0"
        FLINTDB_CHECK_EQ(first_value, std::string("value:0"));

        txm.Commit(reader_txn);  // only now can the writer proceed
    });

    FLINTDB_CHECK(WaitFor(reader_task, kTimeout));
    FLINTDB_CHECK(WaitFor(writer_task, kTimeout));

    auto final_rows = heap.Scan();
    FLINTDB_CHECK_EQ(final_rows.size(), 1u);
    FLINTDB_CHECK_EQ(final_rows[0].second, std::string("value:1"));  // the writer's update did land, just only after
}

FLINTDB_TEST(isolation_phantom_read_is_prevented_when_a_full_scan_has_already_locked_every_existing_page) {
    // A phantom is a row that appears inside a transaction's own
    // repeated reads that wasn't there on an earlier one. HeapFile::Scan()
    // walks and shared-locks *every* page in the table's current
    // page_ids_ snapshot, including whichever page is currently last (the
    // only page an Insert() can land a new row on without first
    // allocating a fresh page -- see HeapFile::Insert()). Because
    // Insert() always tries to exclusively lock that *current last page*
    // before ever checking whether it has room, a concurrent insert
    // cannot get in at all while a full-table scan is holding that page's
    // shared lock -- not even one that would ultimately need a brand-new
    // page (see the next test for the one case where that's not true).
    //
    // The writer is deliberately begun *before* the scanner (older) so
    // it's the one that waits below rather than dying -- see the file
    // comment.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&log);
    txm.RegisterObject(0, &bp);
    HeapFile heap(0, &bp);

    heap.Insert("row-1");  // no transaction active yet -- page 0 has room to spare

    std::atomic<bool> writer_begun{false};
    std::atomic<bool> scanner_locked{false};
    std::atomic<bool> writer_returned{false};

    BackgroundResult writer_task = RunInBackground([&] {
        Transaction* writer_txn = txm.Begin();  // begins first -- older
        writer_begun = true;
        SpinUntil(scanner_locked);
        heap.Insert("phantom-row");  // must block -- the scanner (younger) already holds this page's shared lock
        writer_returned = true;
        txm.Commit(writer_txn);
    });

    SpinUntil(writer_begun);

    BackgroundResult scanner_task = RunInBackground([&] {
        Transaction* scanner_txn = txm.Begin();  // begins second -- younger
        auto first = heap.Scan();
        FLINTDB_CHECK_EQ(first.size(), 1u);
        scanner_locked = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // give the writer time to actually block
        FLINTDB_CHECK(!writer_returned.load());  // still blocked

        auto second = heap.Scan();  // second read, same transaction -- must see no phantom
        FLINTDB_CHECK_EQ(second.size(), 1u);

        txm.Commit(scanner_txn);  // only now can the writer's insert proceed
    });

    FLINTDB_CHECK(WaitFor(scanner_task, kTimeout));
    FLINTDB_CHECK(WaitFor(writer_task, kTimeout));
    FLINTDB_CHECK(writer_returned.load());

    auto final_rows = heap.Scan();
    FLINTDB_CHECK_EQ(final_rows.size(), 2u);  // the insert did land, just only after the scanner's txn ended
}

FLINTDB_TEST(isolation_phantom_read_gap_a_find_that_returns_early_never_locks_the_pages_it_never_reached) {
    // The honest, documented gap (see docs/SPEC.md section 3's phantom-read
    // row): HeapFile::Find() stops and returns as soon as it hits a match,
    // locking only the pages it actually walked to get there -- unlike
    // Scan(), which always walks (and locks) every page. If a match is
    // found on an early page, any *later* page is never touched, so
    // nothing stops a concurrent insert from landing there -- and a
    // second Find()/Scan() later in the same transaction can then observe
    // a row that didn't exist (or wasn't visible) on the first one.
    //
    // This is verified, not assumed: an earlier version of this test
    // tried to reproduce the gap through a full Scan() (matching an
    // earlier draft of SPEC.md's wording, "a phantom on a brand-new page
    // after a scan"), and that scenario turned out not to be reachable at
    // all -- see isolation_phantom_read_is_prevented_when_a_full_scan_...
    // above and D-023 in docs/DECISIONS.md for how that was caught and
    // SPEC.md corrected to describe the mechanism actually demonstrated
    // here instead.
    TempFile db_tmp;
    TempFile wal_tmp("wal");
    DiskManager dm(db_tmp.path());
    BufferPool bp(&dm);
    LogManager log(wal_tmp.path());
    TransactionManager txm(&log);
    txm.RegisterObject(0, &bp);
    HeapFile heap(0, &bp);

    // Build a table with (a) "target" sitting alone on page 0, findable on
    // the very first page Find() ever looks at, and (b) a second,
    // *pre-existing* page already in page_ids_ before any transaction
    // begins, with room to spare and nothing about it needed to find
    // "target" -- so a Find() for "target" never has a reason to touch
    // it. Page 0 is filled to true capacity (FreeSpace() below the
    // smallest possible record) first, purely to force HeapFile's normal
    // "insert onto the current page, else allocate a new one" logic to
    // actually create that second page, non-transactionally, before
    // either transaction below starts.
    PageId first_page = heap.Insert("target").page_id;
    std::string filler(1, '\0');
    while (true) {
        Page* page = bp.FetchPage(first_page);
        if (page->FreeSpace() < Page::kSlotSize + 1) break;
        heap.Insert(filler);
    }
    heap.Insert(filler);  // one more -- this is the one that actually overflows onto the new second page

    std::atomic<bool> reader_first_find_done{false};
    std::atomic<bool> writer_committed{false};

    BackgroundResult reader_task = RunInBackground([&] {
        Transaction* reader_txn = txm.Begin();
        auto first = heap.Find([](const std::string& s) { return s == "target"; });
        FLINTDB_CHECK(first.has_value());
        FLINTDB_CHECK_EQ(first->page_id, first_page);  // found on the very first page -- never reached page 2
        reader_first_find_done = true;

        SpinUntil(writer_committed);  // the writer's insert (below) fully lands and commits in between

        // Second read, same still-open transaction: this is the anomaly
        // -- a row now exists that wasn't visible a moment ago, and
        // nothing blocked it from appearing.
        auto second = heap.Find([](const std::string& s) { return s == "phantom"; });
        FLINTDB_CHECK(second.has_value());  // the gap: this "should" be impossible under serializable isolation
        txm.Commit(reader_txn);
    });

    SpinUntil(reader_first_find_done);

    BackgroundResult writer_task = RunInBackground([&] {
        Transaction* writer_txn = txm.Begin();
        auto start = std::chrono::steady_clock::now();
        heap.Insert("phantom");  // lands on the second, pre-existing page -- never locked by the reader
        auto elapsed = std::chrono::steady_clock::now() - start;
        // Proves this never blocked at all (as opposed to having blocked
        // and then been released quickly) -- the reader's still-open
        // transaction never held any lock this could conflict with.
        FLINTDB_CHECK(elapsed < std::chrono::milliseconds(200));
        txm.Commit(writer_txn);
        writer_committed = true;
    });

    FLINTDB_CHECK(WaitFor(writer_task, kTimeout));
    FLINTDB_CHECK(WaitFor(reader_task, kTimeout));
}
