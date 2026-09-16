#include "database.h"

#include "../wal/recovery.h"

namespace flintdb {

Database::Database(std::string dir_path)
    : dir_path_(std::move(dir_path)),
      catalog_(dir_path_),
      log_manager_(dir_path_ + "/wal.log"),
      txn_manager_(&log_manager_) {
    // ---- Phase 1 (see the class comment, and D-041 for why this must be
    // its own pass, completed in full, before any BufferPool exists) ----
    // Construct a bare DiskManager for every object the Catalog already
    // knows about (a table's heap, or any index -- PK or secondary), and
    // run recovery against the raw ObjectId -> DiskManager* map. Ownership
    // stays in `disk_managers` here so Phase 2 below can hand each one off
    // to its object's real storage without reopening the file a second
    // time.
    std::unordered_map<ObjectId, std::unique_ptr<DiskManager>> disk_managers;
    for (const TableInfo& table : catalog_.Tables()) {
        disk_managers.emplace(table.heap_object_id, std::make_unique<DiskManager>(catalog_.FilePathFor(table.heap_object_id)));
    }
    for (const IndexInfo& index : catalog_.Indexes()) {
        disk_managers.emplace(index.object_id, std::make_unique<DiskManager>(catalog_.FilePathFor(index.object_id)));
    }

    std::unordered_map<ObjectId, DiskManager*> disk_manager_ptrs;
    disk_manager_ptrs.reserve(disk_managers.size());
    for (const auto& [object_id, disk_manager] : disk_managers) {
        disk_manager_ptrs.emplace(object_id, disk_manager.get());
    }
    RunRecovery(log_manager_.RecordsOnOpen(), disk_manager_ptrs);

    // ---- Phase 2: every file above is now fully caught up, so it's safe
    // to layer BufferPool/HeapFile/BPlusTree on top and register each
    // with the shared TransactionManager. ----
    for (const TableInfo& table : catalog_.Tables()) {
        AttachHeapStorage(table.heap_object_id, std::move(disk_managers.at(table.heap_object_id)));
    }
    for (const IndexInfo& index : catalog_.Indexes()) {
        AttachIndexStorage(index.object_id, std::move(disk_managers.at(index.object_id)));
    }
}

void Database::AttachHeapStorage(ObjectId object_id, std::unique_ptr<DiskManager> disk_manager) {
    ObjectStorage storage;
    storage.disk_manager = std::move(disk_manager);
    storage.buffer_pool = std::make_unique<BufferPool>(storage.disk_manager.get());
    storage.heap_file = std::make_unique<HeapFile>(object_id, storage.buffer_pool.get());
    txn_manager_.RegisterObject(object_id, storage.buffer_pool.get());
    objects_.emplace(object_id, std::move(storage));
}

void Database::AttachIndexStorage(ObjectId object_id, std::unique_ptr<DiskManager> disk_manager) {
    ObjectStorage storage;
    storage.disk_manager = std::move(disk_manager);
    storage.buffer_pool = std::make_unique<BufferPool>(storage.disk_manager.get());
    storage.btree = std::make_unique<BPlusTree>(object_id, storage.buffer_pool.get());
    txn_manager_.RegisterObject(object_id, storage.buffer_pool.get());
    objects_.emplace(object_id, std::move(storage));
}

ObjectId Database::CreateTable(const std::string& name, std::vector<ColumnDef> columns,
                                std::optional<std::string> primary_key_column) {
    // Catalog::CreateTable validates and persists first -- if it throws
    // (bad name/columns/PK column), nothing below runs and no storage is
    // ever attached, so a rejected CreateTable leaves Database exactly as
    // it was.
    ObjectId heap_id = catalog_.CreateTable(name, std::move(columns), primary_key_column);
    AttachHeapStorage(heap_id, std::make_unique<DiskManager>(catalog_.FilePathFor(heap_id)));

    if (primary_key_column.has_value()) {
        // Catalog::CreateTable already created the implicit PK IndexInfo
        // (docs/DECISIONS.md D-040) as part of the call above; look it
        // back up to get the ObjectId it was assigned.
        const IndexInfo* pk = catalog_.PrimaryKeyIndex(name);
        AttachIndexStorage(pk->object_id, std::make_unique<DiskManager>(catalog_.FilePathFor(pk->object_id)));
    }
    return heap_id;
}

ObjectId Database::CreateIndex(const std::string& name, const std::string& table_name,
                                const std::string& column_name, const std::function<void(BPlusTree&)>& populate) {
    ObjectId object_id = catalog_.CreateIndex(name, table_name, column_name);
    AttachIndexStorage(object_id, std::make_unique<DiskManager>(catalog_.FilePathFor(object_id)));
    if (populate) {
        ObjectStorage& storage = objects_.at(object_id);
        populate(*storage.btree);
        // D-048: a backfill runs as DDL, never inside a transaction, so
        // nothing else will ever flush these pages -- force it now, the
        // same reasoning as BPlusTree's own constructor (D-043). Safe as
        // FlushAll specifically because this pool backs a file nothing
        // else has ever touched yet.
        storage.buffer_pool->FlushAll();
    }
    return object_id;
}

HeapFile* Database::GetHeapFile(ObjectId object_id) { return objects_.at(object_id).heap_file.get(); }

BPlusTree* Database::GetIndex(ObjectId object_id) { return objects_.at(object_id).btree.get(); }

HeapFile* Database::GetHeapFileForTable(const std::string& table_name) {
    const TableInfo* table = catalog_.FindTable(table_name);
    if (table == nullptr) return nullptr;
    return GetHeapFile(table->heap_object_id);
}

BPlusTree* Database::GetIndexByName(const std::string& index_name) {
    const IndexInfo* index = catalog_.FindIndex(index_name);
    if (index == nullptr) return nullptr;
    return GetIndex(index->object_id);
}

}  // namespace flintdb
