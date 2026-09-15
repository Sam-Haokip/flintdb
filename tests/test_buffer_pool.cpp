#include "../src/storage/buffer_pool.h"
#include "test_framework.h"
#include "test_utils.h"

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
