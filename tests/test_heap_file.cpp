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
    HeapFile heap(&bp);

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
    HeapFile heap(&bp);

    RID r0 = heap.Insert("row-0");
    RID r1 = heap.Insert("row-1");
    FLINTDB_CHECK(r0 != r1);
}

FLINTDB_TEST(heap_file_spans_multiple_pages_when_rows_dont_fit_in_one) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(&bp);

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
    HeapFile heap(&bp);

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
    HeapFile heap(&bp);

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
    HeapFile heap(&bp);

    for (int i = 0; i < 10; i++) {
        heap.Insert("row-" + std::to_string(i));
    }

    // HeapFile::Delete rewrites the whole file and reassigns RIDs for every
    // surviving row on each call (documented in heap_file.h /
    // docs/DECISIONS.md) — a RID captured before an earlier Delete is stale
    // by the time a later Delete runs. So each target's RID has to be
    // looked up fresh, right before it's deleted, exactly as a real caller
    // would have to.
    auto find_rid_by_content = [&](const std::string& content) -> RID {
        for (auto& [rid, bytes] : heap.Scan()) {
            if (bytes == content) return rid;
        }
        throw std::runtime_error("row not found: " + content);
    };

    // Delete every even-indexed row.
    for (int i = 0; i < 10; i += 2) {
        std::string target = "row-" + std::to_string(i);
        FLINTDB_CHECK(heap.Delete(find_rid_by_content(target)));
    }

    auto rows = heap.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 5u);
    std::multiset<std::string> expected = {"row-1", "row-3", "row-5", "row-7", "row-9"};
    FLINTDB_CHECK(RowContents(rows) == expected);
}

FLINTDB_TEST(heap_file_persists_across_reopen_via_a_new_buffer_pool) {
    TempFile tmp;
    {
        DiskManager dm(tmp.path());
        BufferPool bp(&dm);
        HeapFile heap(&bp);
        heap.Insert("durable-ish-row");
        bp.FlushAll();
    }
    DiskManager dm2(tmp.path());
    BufferPool bp2(&dm2);
    HeapFile heap2(&bp2);  // constructor discovers the existing pages
    auto rows = heap2.Scan();
    FLINTDB_CHECK_EQ(rows.size(), 1u);
    FLINTDB_CHECK_EQ(rows[0].second, std::string("durable-ish-row"));
}

FLINTDB_TEST(heap_file_insert_rejects_a_row_bigger_than_a_page_can_ever_hold) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    HeapFile heap(&bp);

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
