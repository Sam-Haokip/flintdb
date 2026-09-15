#include "../src/storage/buffer_pool.h"
#include "test_framework.h"
#include "test_utils.h"

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
