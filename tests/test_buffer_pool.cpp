#include "../src/storage/buffer_pool.h"
#include "test_framework.h"
#include "test_utils.h"

#include <set>
#include <thread>
#include <vector>

using namespace flintdb;
using flintdb::testing::TempFile;

FLINTDB_TEST(buffer_pool_new_page_is_immediately_usable) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    FLINTDB_CHECK(page != nullptr);
    auto slot = page->InsertRecord("row-in-fresh-page");
    FLINTDB_CHECK(slot.has_value());
    FLINTDB_CHECK_EQ(bp.NumPagesOnDisk(), 1u);
}

FLINTDB_TEST(buffer_pool_fetch_returns_the_same_in_memory_page_no_eviction) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    page->InsertRecord("cached");

    Page* fetched_again = bp.FetchPage(pid);
    FLINTDB_CHECK(fetched_again == page);  // identical pointer: no eviction happened
    FLINTDB_CHECK_EQ(fetched_again->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(bp.NumPagesInMemory(), 1u);
}

FLINTDB_TEST(buffer_pool_flush_persists_dirty_pages_to_disk) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid;
    {
        BufferPool bp(&dm);
        Page* page = bp.NewHeapPage(&pid);
        page->InsertRecord("must-survive-flush");
        bp.MarkDirty(pid);
        bp.FlushAll();
    }
    // A second, independent buffer pool over the same disk manager: the
    // page is not in memory yet, so this proves the data actually made it
    // to disk rather than just living in the first pool's cache.
    BufferPool bp2(&dm);
    Page* reloaded = bp2.FetchPage(pid);
    FLINTDB_CHECK_EQ(reloaded->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(reloaded->GetRecord(0), std::string("must-survive-flush"));
}

FLINTDB_TEST(buffer_pool_reset_all_clears_cache_and_truncates_disk) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    PageId pid;
    bp.NewHeapPage(&pid);
    FLINTDB_CHECK_EQ(bp.NumPagesOnDisk(), 1u);

    bp.ResetAll();
    FLINTDB_CHECK_EQ(bp.NumPagesOnDisk(), 0u);
    FLINTDB_CHECK_EQ(bp.NumPagesInMemory(), 0u);

    PageId pid2;
    bp.NewHeapPage(&pid2);
    FLINTDB_CHECK_EQ(pid2, 0u);  // numbering restarted
}

FLINTDB_TEST(buffer_pool_active_transaction_observer_fires_on_mark_dirty) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);  // NewHeapPage dirties pid before the observer is set...
    (void)page;

    std::vector<PageId> notified;
    bp.SetActiveTransactionObserver([&](PageId p) { notified.push_back(p); });

    bp.MarkDirty(pid);  // ...so this is the first call the observer actually sees
    FLINTDB_CHECK_EQ(notified.size(), 1u);
    FLINTDB_CHECK_EQ(notified[0], pid);
}

FLINTDB_TEST(buffer_pool_active_transaction_observer_fires_for_new_pages_too) {
    // NewPage/NewHeapPage must route through MarkDirty, not bypass it --
    // otherwise a transaction that inserts into a brand-new page would
    // never learn that page needs an Update record at commit time.
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    std::vector<PageId> notified;
    bp.SetActiveTransactionObserver([&](PageId p) { notified.push_back(p); });

    PageId pid;
    bp.NewHeapPage(&pid);

    FLINTDB_CHECK_EQ(notified.size(), 1u);
    FLINTDB_CHECK_EQ(notified[0], pid);
}

FLINTDB_TEST(buffer_pool_clear_active_transaction_observer_stops_notifications) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    PageId pid;
    bp.NewHeapPage(&pid);

    int calls = 0;
    bp.SetActiveTransactionObserver([&](PageId) { calls++; });
    bp.MarkDirty(pid);
    FLINTDB_CHECK_EQ(calls, 1);

    bp.ClearActiveTransactionObserver();
    bp.MarkDirty(pid);
    FLINTDB_CHECK_EQ(calls, 1);  // no further notifications after clearing
}

FLINTDB_TEST(buffer_pool_observer_is_harmless_when_none_is_registered) {
    // Every pre-Phase-3 caller (HeapFile, the B-tree) marks pages dirty
    // with no observer ever registered -- this must keep working exactly
    // as before.
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    page->InsertRecord("no-observer-here");
    bp.MarkDirty(pid);  // must not throw or crash with no observer set
    FLINTDB_CHECK_EQ(page->GetRecord(0), std::string("no-observer-here"));
}

FLINTDB_TEST(buffer_pool_discard_page_reverts_in_memory_content_to_disk_content) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    page->InsertRecord("durable-row");
    bp.MarkDirty(pid);
    bp.FlushAll();  // "durable-row" is now what's actually on disk

    page->InsertRecord("never-should-survive");
    FLINTDB_CHECK_EQ(page->GetSlotCount(), 2u);

    bp.DiscardPage(pid);

    // The *same* Page* must now reflect only what was on disk.
    FLINTDB_CHECK_EQ(page->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(page->GetRecord(0), std::string("durable-row"));
}

FLINTDB_TEST(buffer_pool_discard_page_reverts_a_never_flushed_new_page_to_all_zero) {
    // A page that was NewPage'd (so DiskManager already zero-filled it on
    // disk via AllocatePage) but never flushed: discarding it must revert
    // to that pristine zero state, exactly what an abort of an insert
    // into a brand-new page needs.
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    page->InsertRecord("uncommitted");
    FLINTDB_CHECK_EQ(page->GetSlotCount(), 1u);

    bp.DiscardPage(pid);

    FLINTDB_CHECK_EQ(page->GetSlotCount(), 0u);
}

FLINTDB_TEST(buffer_pool_discard_page_on_an_uncached_page_id_is_a_harmless_no_op) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    bp.DiscardPage(999);  // never allocated, never cached -- must not throw
    FLINTDB_CHECK_EQ(bp.NumPagesInMemory(), 0u);
}

FLINTDB_TEST(buffer_pool_discard_page_clears_the_dirty_flag_so_flush_all_skips_it) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    Page* page = bp.NewHeapPage(&pid);
    page->InsertRecord("will-be-discarded");
    bp.DiscardPage(pid);

    // FlushAll must not try to write this page's (now-reverted) content
    // back out as if it were still dirty; NumPagesOnDisk from the earlier
    // AllocatePage already accounts for the page existing, so the real
    // check is that FlushAll runs cleanly and disk content still matches
    // the all-zero state DiscardPage reverted to.
    bp.FlushAll();
    BufferPool bp2(&dm);
    Page* reloaded = bp2.FetchPage(pid);
    FLINTDB_CHECK_EQ(reloaded->GetSlotCount(), 0u);
}

FLINTDB_TEST(buffer_pool_flush_pages_only_writes_the_named_pages_leaving_others_dirty_on_disk) {
    // Phase 4: this is the direct unit-level proof of the FlushPages fix
    // for the FlushAll-over-flush bug (see docs/DECISIONS.md and
    // TransactionManager::Commit) -- two dirty pages, flush only one by
    // name, and confirm the *other* one's on-disk content is untouched
    // (still the all-zero content AllocatePage's ftruncate produced)
    // until it's flushed on its own.
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId page_a, page_b;
    Page* a = bp.NewHeapPage(&page_a);
    Page* b = bp.NewHeapPage(&page_b);
    a->InsertRecord("belongs-to-a");
    b->InsertRecord("belongs-to-b");
    bp.MarkDirty(page_a);
    bp.MarkDirty(page_b);

    bp.FlushPages({page_a});  // only A, never B

    BufferPool bp2(&dm);  // independent pool -- only sees what's actually on disk
    Page* a_reloaded = bp2.FetchPage(page_a);
    Page* b_reloaded = bp2.FetchPage(page_b);
    FLINTDB_CHECK_EQ(a_reloaded->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(a_reloaded->GetRecord(0), std::string("belongs-to-a"));
    FLINTDB_CHECK_EQ(b_reloaded->GetSlotCount(), 0u);  // still the pristine zero-filled page on disk

    // B is still dirty in the *original* pool and can still be flushed on
    // its own -- FlushPages(A) must not have silently cleared B's flag.
    bp.FlushPages({page_b});
    BufferPool bp3(&dm);
    Page* b_reloaded_again = bp3.FetchPage(page_b);
    FLINTDB_CHECK_EQ(b_reloaded_again->GetSlotCount(), 1u);
    FLINTDB_CHECK_EQ(b_reloaded_again->GetRecord(0), std::string("belongs-to-b"));
}

FLINTDB_TEST(buffer_pool_flush_pages_ignores_page_ids_that_are_not_currently_dirty) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    PageId pid;
    bp.NewHeapPage(&pid);
    bp.DiscardPage(pid);  // clears the dirty flag NewHeapPage set

    bp.FlushPages({pid});  // must be a harmless no-op, not throw
    bp.FlushPages({9999}); // a page id never even allocated -- also harmless
}

FLINTDB_TEST(buffer_pool_observer_registry_is_keyed_by_thread_not_a_single_global_callback) {
    // Phase 4: two "transactions" (really just two threads, each with its
    // own registered observer) touching pages concurrently must each
    // only ever be notified about the pages *they themselves* dirtied --
    // proving the thread-keyed registry replacement for Phase 3's single
    // global callback actually isolates threads from each other rather
    // than the last SetActiveTransactionObserver call winning globally.
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    constexpr int kThreads = 6;
    constexpr int kPagesPerThread = 30;
    std::vector<std::vector<PageId>> observed(kThreads);
    std::vector<std::vector<PageId>> created(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bp, &observed, &created, t] {
            bp.SetActiveTransactionObserver([&observed, t](PageId pid) { observed[t].push_back(pid); });
            for (int i = 0; i < kPagesPerThread; ++i) {
                PageId pid;
                bp.NewHeapPage(&pid);  // dirties pid -- must route to *this* thread's observer only
                created[t].push_back(pid);
            }
            bp.ClearActiveTransactionObserver();
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        std::set<PageId> created_set(created[t].begin(), created[t].end());
        std::set<PageId> observed_set(observed[t].begin(), observed[t].end());
        FLINTDB_CHECK_EQ(created_set.size(), static_cast<size_t>(kPagesPerThread));
        // Every page this thread created must appear in what it observed,
        // and nothing else -- no cross-thread leakage either direction.
        FLINTDB_CHECK(created_set == observed_set);
    }
}

FLINTDB_TEST(buffer_pool_concurrent_new_page_never_hands_out_a_duplicate_id_or_corrupts_the_cache) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    std::vector<std::vector<PageId>> results(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bp, &results, t] {
            results[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                PageId pid;
                bp.NewHeapPage(&pid);
                results[t].push_back(pid);
            }
        });
    }
    for (auto& th : threads) th.join();

    std::set<PageId> all_ids;
    for (auto& per_thread : results) {
        for (PageId id : per_thread) all_ids.insert(id);
    }
    FLINTDB_CHECK_EQ(all_ids.size(), static_cast<size_t>(kThreads * kPerThread));
    FLINTDB_CHECK_EQ(bp.NumPagesInMemory(), static_cast<size_t>(kThreads * kPerThread));
    FLINTDB_CHECK_EQ(bp.NumPagesOnDisk(), static_cast<size_t>(kThreads * kPerThread));
}

FLINTDB_TEST(buffer_pool_concurrent_fetch_of_the_same_cached_page_returns_the_identical_pointer_everywhere) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    PageId pid;
    Page* original = bp.NewHeapPage(&pid);

    constexpr int kThreads = 8;
    std::vector<Page*> fetched(kThreads, nullptr);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bp, &fetched, pid, t] { fetched[t] = bp.FetchPage(pid); });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        FLINTDB_CHECK(fetched[t] == original);  // no eviction, no duplicate Page object
    }
}
