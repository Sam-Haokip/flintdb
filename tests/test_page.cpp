#include "../src/storage/page.h"
#include "test_framework.h"

using namespace flintdb;

FLINTDB_TEST(page_init_starts_empty) {
    Page page;
    page.InitHeapPage(7);
    FLINTDB_CHECK_EQ(page.GetPageId(), 7u);
    FLINTDB_CHECK_EQ(page.GetSlotCount(), 0u);
    FLINTDB_CHECK_EQ(page.FreeSpace(), PAGE_SIZE - Page::kHeaderSize);
}

FLINTDB_TEST(page_insert_and_get_single_record) {
    Page page;
    page.InitHeapPage(0);
    auto slot = page.InsertRecord("hello world");
    FLINTDB_CHECK(slot.has_value());
    FLINTDB_CHECK_EQ(*slot, 0u);
    FLINTDB_CHECK_EQ(page.GetRecord(*slot), std::string("hello world"));
    FLINTDB_CHECK_EQ(page.GetSlotCount(), 1u);
}

FLINTDB_TEST(page_insert_multiple_records_preserves_each_independently) {
    Page page;
    page.InitHeapPage(0);
    auto s0 = page.InsertRecord("first");
    auto s1 = page.InsertRecord("second-record");
    auto s2 = page.InsertRecord("3");
    FLINTDB_CHECK(s0.has_value());
    FLINTDB_CHECK(s1.has_value());
    FLINTDB_CHECK(s2.has_value());
    // Slot ids are assigned in insertion order.
    FLINTDB_CHECK_EQ(*s0, 0u);
    FLINTDB_CHECK_EQ(*s1, 1u);
    FLINTDB_CHECK_EQ(*s2, 2u);
    // Records don't get clobbered by later inserts.
    FLINTDB_CHECK_EQ(page.GetRecord(*s0), std::string("first"));
    FLINTDB_CHECK_EQ(page.GetRecord(*s1), std::string("second-record"));
    FLINTDB_CHECK_EQ(page.GetRecord(*s2), std::string("3"));
    FLINTDB_CHECK_EQ(page.GetSlotCount(), 3u);
}

FLINTDB_TEST(page_insert_empty_string_is_a_valid_zero_length_record) {
    Page page;
    page.InitHeapPage(0);
    auto slot = page.InsertRecord("");
    FLINTDB_CHECK(slot.has_value());
    FLINTDB_CHECK_EQ(page.GetRecord(*slot), std::string(""));
}

FLINTDB_TEST(page_insert_fails_when_record_bigger_than_whole_page) {
    Page page;
    page.InitHeapPage(0);
    std::string big(PAGE_SIZE, 'x');
    auto slot = page.InsertRecord(big);
    FLINTDB_CHECK(!slot.has_value());
    // A rejected insert must not have mutated the page.
    FLINTDB_CHECK_EQ(page.GetSlotCount(), 0u);
    FLINTDB_CHECK_EQ(page.FreeSpace(), PAGE_SIZE - Page::kHeaderSize);
}

FLINTDB_TEST(page_fills_up_and_then_refuses_further_inserts) {
    Page page;
    page.InitHeapPage(0);
    std::string rec(100, 'a');
    int inserted = 0;
    while (page.InsertRecord(rec).has_value()) {
        inserted++;
    }
    FLINTDB_CHECK(inserted > 0);
    // Every one of the records that did fit must still read back correctly.
    for (int i = 0; i < inserted; i++) {
        FLINTDB_CHECK_EQ(page.GetRecord(static_cast<SlotId>(i)), rec);
    }
    // Roughly (PAGE_SIZE - header) / (record + slot overhead) should fit;
    // this is a sanity bound, not an exact model of the layout.
    size_t upper_bound = (PAGE_SIZE - Page::kHeaderSize) / (100 + Page::kSlotSize) + 1;
    FLINTDB_CHECK(static_cast<size_t>(inserted) <= upper_bound);
}
