// Integration tests for the Database facade (docs/DECISIONS.md D-034):
// opening a fresh or existing directory, CreateTable/CreateIndex wiring up
// real storage, cross-object transactional atomicity through the one
// shared TransactionManager, and -- the property D-041 specifically
// exists to guarantee -- that reopening a database whose index storage
// wasn't fully force-flushed before a crash still recovers correctly.

#include "../src/db/database.h"
#include "test_framework.h"
#include "test_utils.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <vector>

using namespace flintdb;
using flintdb::testing::TempDir;

namespace {

// Reads/writes exactly PAGE_SIZE bytes at `page_id`'s offset directly on
// the raw file, entirely bypassing DiskManager/BufferPool -- used only to
// simulate "this specific page's force-flush never reached disk" in the
// D-041 regression test below, mirroring test_utils.h's own
// TruncateFileTo/CorruptByteAt technique of manipulating on-disk bytes
// directly rather than through the objects that normally own them.
std::vector<char> ReadRawPage(const std::string& path, PageId page_id) {
    std::vector<char> buf(PAGE_SIZE);
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("ReadRawPage: open failed");
    ssize_t n = pread(fd, buf.data(), PAGE_SIZE, static_cast<off_t>(page_id) * PAGE_SIZE);
    close(fd);
    if (n != static_cast<ssize_t>(PAGE_SIZE)) throw std::runtime_error("ReadRawPage: short read");
    return buf;
}

void WriteRawPage(const std::string& path, PageId page_id, const std::vector<char>& buf) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) throw std::runtime_error("WriteRawPage: open failed");
    ssize_t n = pwrite(fd, buf.data(), PAGE_SIZE, static_cast<off_t>(page_id) * PAGE_SIZE);
    close(fd);
    if (n != static_cast<ssize_t>(PAGE_SIZE)) throw std::runtime_error("WriteRawPage: short write");
}

}  // namespace

FLINTDB_TEST(database_opening_a_fresh_directory_starts_with_an_empty_catalog) {
    TempDir dir;
    Database db(dir.path());
    FLINTDB_CHECK(db.GetCatalog().Tables().empty());
    FLINTDB_CHECK(db.GetCatalog().Indexes().empty());
}

FLINTDB_TEST(database_create_table_without_primary_key_wires_up_a_working_heap_file) {
    TempDir dir;
    Database db(dir.path());
    db.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, std::nullopt);

    HeapFile* heap = db.GetHeapFileForTable("widgets");
    FLINTDB_CHECK(heap != nullptr);

    TransactionManager& txm = db.GetTransactionManager();
    Transaction* txn = txm.Begin();
    heap->Insert("row-a");
    heap->Insert("row-b");
    txm.Commit(txn);

    FLINTDB_CHECK_EQ(heap->NumRows(), 2u);
    FLINTDB_CHECK(db.GetHeapFileForTable("no_such_table") == nullptr);
    FLINTDB_CHECK(db.GetIndexByName("no_such_index") == nullptr);
}

FLINTDB_TEST(database_create_table_with_primary_key_also_wires_up_a_working_pk_index) {
    TempDir dir;
    Database db(dir.path());
    db.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    const IndexInfo* pk = db.GetCatalog().PrimaryKeyIndex("widgets");
    FLINTDB_CHECK(pk != nullptr);
    BPlusTree* index = db.GetIndex(pk->object_id);
    FLINTDB_CHECK(index != nullptr);
    FLINTDB_CHECK(index == db.GetIndexByName(pk->name));

    HeapFile* heap = db.GetHeapFileForTable("widgets");
    TransactionManager& txm = db.GetTransactionManager();

    // A single transaction maintaining both the heap row and its PK index
    // entry together -- exactly the "one statement, two objects, one
    // atomic unit" scenario D-032 exists for.
    Transaction* txn = txm.Begin();
    RID rid = heap->Insert("row-with-pk-1");
    index->Insert(1, rid);
    txm.Commit(txn);

    auto found = index->Search(1);
    FLINTDB_CHECK_EQ(found.size(), 1u);
    FLINTDB_CHECK(found[0] == rid);
}

FLINTDB_TEST(database_create_index_on_an_existing_table_wires_up_a_working_secondary_index) {
    TempDir dir;
    Database db(dir.path());
    db.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"qty", ColumnType::kInteger}}, "id");
    db.CreateIndex("widgets_qty_idx", "widgets", "qty");

    BPlusTree* qty_index = db.GetIndexByName("widgets_qty_idx");
    FLINTDB_CHECK(qty_index != nullptr);

    TransactionManager& txm = db.GetTransactionManager();
    Transaction* txn = txm.Begin();
    RID rid = db.GetHeapFileForTable("widgets")->Insert("row-1");
    qty_index->Insert(42, rid);
    txm.Commit(txn);

    FLINTDB_CHECK_EQ(qty_index->Search(42).size(), 1u);
}

FLINTDB_TEST(database_get_heap_file_or_index_by_unknown_object_id_throws) {
    TempDir dir;
    Database db(dir.path());
    bool threw = false;
    try {
        db.GetHeapFile(999);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    threw = false;
    try {
        db.GetIndex(999);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(database_a_transaction_touching_two_objects_together_aborts_both_together) {
    // Proves cross-object atomicity actually holds, not just that both
    // objects happen to be reachable: an aborted transaction that touched
    // both the heap and its PK index leaves NEITHER changed.
    TempDir dir;
    Database db(dir.path());
    db.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
    HeapFile* heap = db.GetHeapFileForTable("widgets");
    BPlusTree* index = db.GetIndexByName(db.GetCatalog().PrimaryKeyIndex("widgets")->name);

    TransactionManager& txm = db.GetTransactionManager();
    Transaction* txn = txm.Begin();
    RID rid = heap->Insert("will-be-rolled-back");
    index->Insert(7, rid);
    txm.Abort(txn);

    FLINTDB_CHECK_EQ(heap->NumRows(), 0u);
    FLINTDB_CHECK(index->Search(7).empty());
}

FLINTDB_TEST(database_reopening_persists_schema_heap_rows_and_index_entries) {
    TempDir dir;
    ObjectId heap_id, pk_object_id;
    RID rid;
    {
        Database db(dir.path());
        heap_id = db.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");
        pk_object_id = db.GetCatalog().PrimaryKeyIndex("widgets")->object_id;

        HeapFile* heap = db.GetHeapFileForTable("widgets");
        BPlusTree* index = db.GetIndex(pk_object_id);
        TransactionManager& txm = db.GetTransactionManager();
        Transaction* txn = txm.Begin();
        rid = heap->Insert("persisted-row");
        index->Insert(100, rid);
        txm.Commit(txn);
        // "Crash": db goes out of scope with no explicit final flush
        // beyond what Commit already did internally (force policy, D-020).
    }

    Database reopened(dir.path());
    const TableInfo* table = reopened.GetCatalog().FindTable("widgets");
    FLINTDB_CHECK(table != nullptr);
    FLINTDB_CHECK_EQ(table->heap_object_id, heap_id);

    HeapFile* heap2 = reopened.GetHeapFileForTable("widgets");
    FLINTDB_CHECK_EQ(heap2->NumRows(), 1u);
    auto rows = heap2->Scan();
    FLINTDB_CHECK_EQ(rows.size(), 1u);
    FLINTDB_CHECK_EQ(rows[0].second, std::string("persisted-row"));

    BPlusTree* index2 = reopened.GetIndex(pk_object_id);
    auto found = index2->Search(100);
    FLINTDB_CHECK_EQ(found.size(), 1u);
    FLINTDB_CHECK(found[0] == rid);
}

FLINTDB_TEST(
    database_reopen_recovers_an_index_root_split_whose_force_flush_never_reached_disk_D041) {
    // The exact bug docs/DECISIONS.md D-041 documents and fixes: if a
    // BPlusTree's header page (page 0, D-036) is stale on disk relative
    // to what the WAL says committed, RunRecovery must fix that *before*
    // any BufferPool -- including the one inside BPlusTree's own
    // constructor -- ever reads it, or the reopened tree silently keeps
    // using the wrong root pointer forever. This test manufactures
    // exactly that staleness directly on the file (mirroring
    // test_utils.h's TruncateFileTo/CorruptByteAt technique) rather than
    // relying on timing, so it's deterministic.
    TempDir dir;
    ObjectId pk_object_id;
    std::string index_file_path;
    std::vector<char> pre_split_header;
    constexpr int64_t kBeforeSplit = 200;  // comfortably under one leaf's ~291-key capacity
    constexpr int64_t kAfterSplit = 320;   // comfortably past it -- forces a root split
    {
        Database db(dir.path());
        db.CreateTable("widgets", {{"id", ColumnType::kInteger}}, "id");
        const IndexInfo* pk = db.GetCatalog().PrimaryKeyIndex("widgets");
        pk_object_id = pk->object_id;
        index_file_path = db.GetCatalog().FilePathFor(pk_object_id);
        BPlusTree* index = db.GetIndex(pk_object_id);
        TransactionManager& txm = db.GetTransactionManager();

        // Commit 1: stay within a single leaf -- root_page_id_ still
        // points directly at that leaf, no split yet.
        Transaction* t1 = txm.Begin();
        for (int64_t k = 0; k < kBeforeSplit; k++) index->Insert(k, RID{static_cast<PageId>(k), 0});
        txm.Commit(t1);
        FLINTDB_CHECK_EQ(index->Height(), 1u);
        pre_split_header = ReadRawPage(index_file_path, 0);  // snapshot the pre-split header

        // Commit 2: push past the leaf's capacity -- this splits the
        // root. Commit() force-flushes (D-020), so right now the file's
        // page 0 genuinely holds the *post*-split header.
        Transaction* t2 = txm.Begin();
        for (int64_t k = kBeforeSplit; k < kAfterSplit; k++) index->Insert(k, RID{static_cast<PageId>(k), 0});
        txm.Commit(t2);
        FLINTDB_CHECK(index->Height() > 1);

        // Simulate "this object's force-flush of page 0 specifically
        // never reached disk before the crash" by reverting just that
        // page back to its pre-split bytes. The WAL (already fsynced by
        // Commit) still correctly has an Update record for page 0's
        // post-split content -- that's what recovery must use to fix
        // this back up.
        WriteRawPage(index_file_path, 0, pre_split_header);
    }  // "crash": db destructed with page 0 deliberately stale on disk

    // Sanity check the simulation actually did what it claims: without
    // recovery, the file itself would show the tree as unsplit.
    FLINTDB_CHECK(ReadRawPage(index_file_path, 0) == pre_split_header);

    Database reopened(dir.path());
    BPlusTree* recovered_index = reopened.GetIndex(pk_object_id);
    FLINTDB_CHECK(recovered_index->Height() > 1);  // recovery restored the post-split header
    for (int64_t k = 0; k < kAfterSplit; k++) {
        auto results = recovered_index->Search(k);
        FLINTDB_CHECK_EQ(results.size(), 1u);
        FLINTDB_CHECK((results[0] == RID{static_cast<PageId>(k), 0}));
    }
}
