// Storage-corruption blast-radius tests (task #54).
//
// docs/SPEC.md is explicit that FlintDB has no per-page checksum
// (storage/page.h's page format has no checksum field at all -- confirmed
// by inspection before any of this was written) -- corruption *detection*
// was never a goal here, and that is a deliberate, documented trade-off
// this file does not relitigate. What it checks instead is *blast
// radius*: when a byte on disk is silently corrupted -- bit rot, a bad
// sector, anything outside this engine's own control -- does the damage
// stay contained to the one object it actually landed in, does an
// out-of-range corrupted pointer fail cleanly rather than crash the
// process, and (the question this project's own history makes worth
// checking directly rather than assuming, given D-036/D-041/D-042/D-043 --
// three separate real bugs, one of them a genuine infinite loop, all
// rooted in exactly this class of stale/wrong page pointer) does an
// *in-range* corrupted child pointer that happens to form a cycle hang a
// traversal forever? See docs/DECISIONS.md D-054 for what the last
// question found here and the fix it led to in src/index/btree.cpp.
#include "../src/common/config.h"
#include "../src/db/database.h"
#include "../src/sql/parser.h"
#include "../src/sql/session.h"
#include "../src/txn/transaction.h"
#include "test_framework.h"
#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {

ExecuteResult Run(Database& db, const std::string& sql) { return Execute(db, Parse(sql)); }

// Mirrors src/index/btree.cpp's own "On-disk node layout" comment
// exactly (PageId at offset 0, a PageType tag at offset 4, a 16-byte
// header, child_0 immediately after it) -- duplicated here deliberately:
// a test reading the *actual on-disk format* a corrupted disk would
// present should hardcode the format it's testing against, not reach
// into btree.cpp's own anonymous-namespace constants.
constexpr size_t kOffPageType = 4;
constexpr size_t kBTreeHeaderSize = 16;

// Scans `path` (a BPlusTree's own file) page by page, skipping page 0
// (the root-pointer header page, docs/DECISIONS.md D-036), and returns
// the PageId of the first internal node found. Assumes the tree has
// exactly one internal node, true immediately after exactly one leaf
// split -- which is all every test below needs -- not a general
// tree-walking utility.
PageId FindFirstInternalNodePage(const std::string& path) {
    size_t file_size = flintdb::testing::FileSizeOf(path);
    size_t num_pages = file_size / PAGE_SIZE;
    for (PageId pid = 1; pid < static_cast<PageId>(num_pages); pid++) {
        size_t page_offset = static_cast<size_t>(pid) * PAGE_SIZE;
        auto type = flintdb::testing::ReadRawValueAt<uint16_t>(path, page_offset + kOffPageType);
        if (static_cast<PageType>(type) == PageType::kBTreeInternal) return pid;
    }
    throw std::runtime_error("FindFirstInternalNodePage: no internal node found -- did the split not happen?");
}

// Builds a fresh database at `dir`, containing one table with enough
// rows (320, comfortably past a leaf's ~291-key capacity -- the same
// margin tests/test_end_to_end.cpp's own root-split test uses) to force
// exactly one PRIMARY KEY index root split, and returns that index's raw
// file path. The table itself is otherwise unused by callers -- what
// they actually want is a real, on-disk internal node to corrupt.
//
// Calls Database::Checkpoint() before returning -- not just for the
// belt-and-suspenders reason "make sure it's on disk" (commit already
// guarantees that, D-020), but for a real correctness reason specific to
// *this* kind of test, found while writing it: without a checkpoint, the
// WAL still holds every redo record from the bulk insert above, and a
// later reopen's recovery (docs/SPEC.md section 2) faithfully *replays*
// every one of them -- including the record for whichever page a test
// goes on to corrupt on disk between closing this Database and opening
// the next one, silently overwriting the corruption with the original,
// correct content before any test code ever gets to observe it. The
// first version of this test file didn't call Checkpoint() and every
// corruption test in it consequently, and misleadingly, passed for the
// wrong reason (the "corruption" never survived to be read at all) --
// caught by checking what DiskManager::ReadPage's std::out_of_range test
// actually observed, not by assuming a green run meant the test worked.
// Checkpoint() empties the WAL (LogManager::Checkpoint, D-018), so
// there's nothing left for recovery to redo over a corruption made after
// this function returns.
std::string BuildTableWithOneIndexSplit(const std::string& dir, const std::string& table_name) {
    constexpr int64_t kBulkStart = 1000;
    constexpr int64_t kBulkCount = 320;
    Database db(dir);
    Run(db, "CREATE TABLE " + table_name + " (id INTEGER PRIMARY KEY, val TEXT)");
    Transaction* txn = db.GetTransactionManager().Begin();
    for (int64_t k = kBulkStart; k < kBulkStart + kBulkCount; k++) {
        ExecuteInCurrentTransaction(db, Parse("INSERT INTO " + table_name + " VALUES (" + std::to_string(k) +
                                               ", 'x')"));
    }
    db.GetTransactionManager().Commit(txn);
    db.Checkpoint();

    const IndexInfo* pk = db.GetCatalog().PrimaryKeyIndex(table_name);
    FLINTDB_CHECK(pk != nullptr);
    return db.GetCatalog().FilePathFor(pk->object_id);
}

}  // namespace

FLINTDB_TEST(storage_corruption_in_one_table_does_not_affect_an_unrelated_table) {
    // The one-file-per-object architecture (docs/DECISIONS.md D-031/
    // D-032) means two tables' data physically cannot share a byte of
    // storage -- this confirms that empirically, not just by reading the
    // architecture's own design intent.
    TempDir dir;
    std::string corrupted_path;
    {
        Database db(dir.path());
        Run(db, "CREATE TABLE a (id INTEGER PRIMARY KEY, val TEXT)");
        Run(db, "CREATE TABLE b (id INTEGER PRIMARY KEY, val TEXT)");
        Run(db, "INSERT INTO a VALUES (1, 'a-row-1')");
        Run(db, "INSERT INTO a VALUES (2, 'a-row-2')");
        Run(db, "INSERT INTO b VALUES (1, 'b-row-1')");
        Run(db, "INSERT INTO b VALUES (2, 'b-row-2')");

        const TableInfo* table_a = db.GetCatalog().FindTable("a");
        FLINTDB_CHECK(table_a != nullptr);
        corrupted_path = db.GetCatalog().FilePathFor(table_a->heap_object_id);

        // Checkpoint before this scope closes -- see BuildTableWithOneIndexSplit's
        // comment above for the full reasoning (D-054): without this, the WAL
        // still holds redo records for every row just inserted above, and
        // `reopened`'s recovery pass would silently overwrite the corruption
        // below with the original bytes before this test's own reopen ever gets
        // to observe it. This test only asserts on table b, so a healed table a
        // wouldn't make it fail either way, but calling Checkpoint() keeps the
        // test's own claim ("corrupt a byte... deep enough to land inside real
        // row data") actually true rather than accidentally vacuous.
        db.Checkpoint();
    }

    // Corrupt a byte squarely inside table a's own heap file, well past
    // its header -- deep enough to land inside real row data, not a page
    // tag that might make the corruption merely cosmetic.
    size_t file_size = flintdb::testing::FileSizeOf(corrupted_path);
    flintdb::testing::CorruptByteAt(corrupted_path, file_size - 1);

    Database reopened(dir.path());
    ExecuteResult b_rows = Run(reopened, "SELECT * FROM b");
    FLINTDB_CHECK_EQ(b_rows.rows.size(), 2u);
    bool saw_1 = false, saw_2 = false;
    for (const auto& row : b_rows.rows) {
        if (row[0].int_value == 1) {
            saw_1 = true;
            FLINTDB_CHECK_EQ(row[1].string_value, std::string("b-row-1"));
        }
        if (row[0].int_value == 2) {
            saw_2 = true;
            FLINTDB_CHECK_EQ(row[1].string_value, std::string("b-row-2"));
        }
    }
    FLINTDB_CHECK(saw_1);
    FLINTDB_CHECK(saw_2);
}

FLINTDB_TEST(storage_corruption_child_pointer_out_of_file_range_throws_a_clean_error_not_a_crash) {
    // DiskManager::ReadPage already bounds-checks every page_id against
    // the file's actual size (storage/disk_manager.cpp) and throws
    // std::out_of_range rather than reading past the end of the file --
    // this test pins that existing protection down as a regression test
    // specifically for the path a corrupted B+-tree child pointer would
    // take through it, rather than trusting it holds for this case by
    // inspection alone.
    TempDir dir;
    std::string index_path = BuildTableWithOneIndexSplit(dir.path(), "widgets");

    PageId internal_page = FindFirstInternalNodePage(index_path);
    size_t child0_offset = static_cast<size_t>(internal_page) * PAGE_SIZE + kBTreeHeaderSize;
    // Flip the child pointer's highest-order byte (it's a little-endian
    // uint32_t, common/config.h) so the corrupted value is pushed well
    // past four billion -- guaranteed out of range for any file this
    // small, unlike a low-byte flip, which could easily still land on
    // another real page by chance.
    flintdb::testing::CorruptByteAt(index_path, child0_offset + 3);

    Database reopened(dir.path());
    bool threw = false;
    try {
        Run(reopened, "SELECT * FROM widgets WHERE id = 1000");
    } catch (const std::exception&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(
    storage_corruption_child_pointer_forming_a_self_referencing_cycle_is_caught_by_a_bounded_traversal_not_an_infinite_hang) {
    // docs/DECISIONS.md D-054: the case the engine's own history
    // (D-036/D-041/D-042/D-043) makes worth checking directly rather than
    // assuming -- an *in-range* corrupted child pointer, pointed at the
    // internal node's own page, so a traversal into it lands right back
    // where it started, forever, unless something bounds it.
    TempDir dir;
    std::string index_path = BuildTableWithOneIndexSplit(dir.path(), "widgets");

    PageId internal_page = FindFirstInternalNodePage(index_path);
    size_t child0_offset = static_cast<size_t>(internal_page) * PAGE_SIZE + kBTreeHeaderSize;
    flintdb::testing::WriteRawValueAt<PageId>(index_path, child0_offset, internal_page);

    Database reopened(dir.path());

    // A corrupted traversal must not hang the whole test binary if the
    // engine's own bound (D-054) somehow regresses -- run it on its own
    // thread with a generous-but-finite wall-clock budget, since this
    // test has to terminate either way. The worker is deliberately
    // detached, not joined: if the bound below is ever lost again, this
    // thread would spin forever, and joining it would just make this
    // test hang instead of failing cleanly.
    std::promise<void> done;
    std::future<void> done_future = done.get_future();
    std::exception_ptr thrown;
    std::thread worker([&]() {
        try {
            // id = 1000 is the smallest key in the tree (BuildTableWith-
            // OneIndexSplit inserts 1000..1319 in order), so this
            // deterministically routes through the root's children[0] --
            // the exact pointer corrupted above.
            Run(reopened, "SELECT * FROM widgets WHERE id = 1000");
        } catch (...) {
            thrown = std::current_exception();
        }
        done.set_value();
    });
    worker.detach();

    bool completed = done_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    FLINTDB_CHECK(completed);  // must not hang -- this is what D-054's fix exists to guarantee
    if (completed) {
        FLINTDB_CHECK(thrown != nullptr);  // must fail cleanly, not silently return a wrong (or empty) result
    }
}
