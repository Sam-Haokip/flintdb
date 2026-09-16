#include "../src/storage/heap_file.h"
#include "test_framework.h"
#include "test_utils.h"

#include <set>

using namespace flintdb;
using flintdb::testing::TempFile;

namespace {
std::multiset<std::string> RowContents(const std::vector<std::pair<RID, std::string>>& rows) {
    std::multiset<std::string> out;
    for (auto& [rid, bytes] : rows) out.insert(bytes);
    return out;
}
}  // namespace

FLINTDB_TEST(heap_file_insert_then_scan_returns_every_row) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    heap.Insert("alice,30");
    heap.Insert("bob,25");
    heap.Insert("carol,40");

    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 3u);
    std::multiset<std::string> expected = {"alice,30", "bob,25", "carol,40"};
    FLINTDB_CHECK(RowContents(rows) == expected);
    FLINTDB_CHECK_EQ(heap.NumRows(), 3u);
}

FLINTDB_TEST(heap_file_insert_returns_distinct_rids) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID r0 = heap.Insert("row-0");
    RID r1 = heap.Insert("row-1");
    FLINTDB_CHECK(r0 != r1);
}

FLINTDB_TEST(heap_file_get_row_returns_the_bytes_at_a_known_rid) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID r0 = heap.Insert("row-0");
    RID r1 = heap.Insert("row-1");

    auto row0 = heap.GetRow(r0);
    FLINTDB_CHECK(row0.has_value());
    FLINTDB_CHECK_EQ(*row0, std::string("row-0"));

    auto row1 = heap.GetRow(r1);
    FLINTDB_CHECK(row1.has_value());
    FLINTDB_CHECK_EQ(*row1, std::string("row-1"));
}

FLINTDB_TEST(heap_file_get_row_of_unknown_or_deleted_rid_returns_nullopt) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID real = heap.Insert("row-0");

    // Unknown page entirely.
    FLINTDB_CHECK(!heap.GetRow(RID{999, 0}).has_value());
    // Known page, out-of-range slot.
    FLINTDB_CHECK(!heap.GetRow(RID{real.page_id, static_cast<SlotId>(real.slot_id + 1)}).has_value());
    // Deleted slot.
    FLINTDB_CHECK(heap.Delete(real));
    FLINTDB_CHECK(!heap.GetRow(real).has_value());
}

FLINTDB_TEST(heap_file_spans_multiple_pages_when_rows_dont_fit_in_one) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    std::string base(200, 'r');
    const int n = 40;  // 40 * (200 + slot overhead) exceeds one 4KB page
    for (int i = 0; i < n; i++) heap.Insert(base + std::to_string(i));

    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), static_cast<size_t>(n));
    FLINTDB_CHECK_EQ(heap.NumRows(), static_cast<size_t>(n));

    std::set<PageId> distinct_pages;
    for (auto& [rid, bytes] : rows) distinct_pages.insert(rid.page_id);
    FLINTDB_CHECK(distinct_pages.size() > 1);  // must have spilled onto a second page

    // Every row's exact content must still be intact after spilling.
    std::multiset<std::string> expected;
    for (int i = 0; i < n; i++) expected.insert(base + std::to_string(i));
    FLINTDB_CHECK(RowContents(rows) == expected);
}

FLINTDB_TEST(heap_file_delete_removes_exactly_the_target_row) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID r0 = heap.Insert("row-0");
    heap.Insert("row-1");
    heap.Insert("row-2");
    FLINTDB_CHECK_EQ(heap.NumRows(), 3u);

    bool deleted = heap.Delete(r0);
    FLINTDB_CHECK(deleted);

    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 2u);
    std::multiset<std::string> expected = {"row-1", "row-2"};
    FLINTDB_CHECK(RowContents(rows) == expected);
    FLINTDB_CHECK_EQ(heap.NumRows(), 2u);
}

FLINTDB_TEST(heap_file_delete_of_unknown_rid_is_a_no_op) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    heap.Insert("only-row");
    RID bogus{999, 5};
    bool deleted = heap.Delete(bogus);
    FLINTDB_CHECK(!deleted);
    FLINTDB_CHECK_EQ(heap.NumRows(), 1u);
    FLINTDB_CHECK_EQ(heap.Scan()[0].second, std::string("only-row"));
}

FLINTDB_TEST(heap_file_delete_across_many_rows_leaves_exactly_the_survivors) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    // Delete is now tombstone-in-place (D-015): a RID captured before any
    // Delete call stays valid for every later one, unlike the original
    // Phase 1 delete-by-rewrite this replaced. So every RID can simply be
    // cached up front, exactly as a real caller holding onto RIDs (e.g. a
    // B-tree index entry) would expect.
    std::vector<RID> rids;
    for (int i = 0; i < 10; i++) {
        rids.push_back(heap.Insert("row-" + std::to_string(i)));
    }

    // Delete every even-indexed row using the RIDs captured at insert time.
    for (int i = 0; i < 10; i += 2) {
        FLINTDB_CHECK(heap.Delete(rids[static_cast<size_t>(i)]));
    }

    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 5u);
    std::multiset<std::string> expected = {"row-1", "row-3", "row-5", "row-7", "row-9"};
    FLINTDB_CHECK(RowContents(rows) == expected);
}

FLINTDB_TEST(heap_file_delete_is_tombstone_in_place_rid_stays_stable) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID r0 = heap.Insert("row-0");
    RID r1 = heap.Insert("row-1");
    RID r2 = heap.Insert("row-2");

    FLINTDB_CHECK(heap.Delete(r1));

    // r0 and r2 must still resolve to exactly their original content —
    // nothing was renumbered.
    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 2u);
    for (auto& [rid, bytes] : rows) {
        if (rid == r0) FLINTDB_CHECK_EQ(bytes, std::string("row-0"));
        else if (rid == r2) FLINTDB_CHECK_EQ(bytes, std::string("row-2"));
        else FLINTDB_CHECK(false);  // no other RID should appear
    }
}

FLINTDB_TEST(heap_file_delete_of_already_deleted_rid_is_a_no_op) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    RID r0 = heap.Insert("only-row");
    FLINTDB_CHECK(heap.Delete(r0));
    FLINTDB_CHECK(!heap.Delete(r0));  // second delete of the same RID fails
    FLINTDB_CHECK_EQ(heap.NumRows(), 0u);
}

FLINTDB_TEST(heap_file_persists_across_reopen_via_a_new_buffer_pool) {
    TempFile tmp;
    {
        DiskManager dm(tmp.path());
        BufferPool bp(&dm);
        HeapFile heap(0, &bp);
        heap.Insert("durable-ish-row");
        bp.FlushAll();
    }
    DiskManager dm2(tmp.path());
    BufferPool bp2(&dm2);
    HeapFile heap2(0, &bp2);  // constructor discovers the existing pages
    auto rows = heap2.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 1u);
    FLINTDB_CHECK_EQ(rows[0].second, std::string("durable-ish-row"));
}

FLINTDB_TEST(heap_file_insert_rejects_a_row_bigger_than_a_page_can_ever_hold) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(0, &bp);

    std::string too_big(PAGE_SIZE, 'x');
    bool threw = false;
    try {
        heap.Insert(too_big);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK_EQ(heap.NumRows(), 0u);
}
