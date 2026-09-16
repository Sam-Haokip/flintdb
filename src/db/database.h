#pragma once
#include "../catalog/catalog.h"
#include "../common/config.h"
#include "../index/btree.h"
#include "../storage/buffer_pool.h"
#include "../storage/disk_manager.h"
#include "../storage/heap_file.h"
#include "../txn/transaction_manager.h"
#include "../wal/log_manager.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace flintdb {

// The top-level embeddable API for a whole FlintDB database (docs/SPEC.md's
// "a library linked into a host process" -- this is that library's front
// door, docs/DECISIONS.md D-034). Opening a `Database` gets a caller a
// fully wired, already-recovered system: the Catalog, one shared
// LockManager+LogManager+TransactionManager (spanning every table/index so
// one transaction can atomically touch more than one -- D-032), and every
// table/index's own DiskManager+BufferPool+HeapFile/BPlusTree, each already
// registered with the TransactionManager.
//
// Directory layout (everything lives under the one `dir_path` passed to
// the constructor): `catalog.meta` (Catalog's own file, D-033),
// `wal.log` (the one shared WAL, D-032), and `obj_<id>.dat` per table/
// index (Catalog::FilePathFor, D-031). A `Database` owns all of it --
// nothing outside this class opens any of these files directly.
//
// Opening an *existing* database directory has a strict two-phase order
// that is NOT "read catalog, build every object's full storage, then
// recover" (which is what an earlier draft of D-034 said, and which is
// wrong -- see D-041 for the exact bug that ordering causes):
//
//   Phase 1: read the Catalog, then construct a bare DiskManager for
//   every ObjectId it lists (no BufferPool yet) and run RunRecovery
//   against the resulting ObjectId -> DiskManager* map. This brings
//   every object's *file* fully up to date with everything the WAL says
//   committed, using nothing but raw DiskManager::WritePage calls --
//   there is no BufferPool anywhere yet for a replayed page to go stale
//   in.
//
//   Phase 2: only now, wrap each already-recovered DiskManager in a
//   fresh BufferPool, construct that object's HeapFile or BPlusTree on
//   top of it (safe now -- BPlusTree's constructor may itself call
//   BufferPool::FetchPage to read back a persisted root pointer, D-036,
//   and by this point that page is guaranteed to already hold the fully
//   recovered content), and register it with the TransactionManager.
//
// Opening a *fresh* (empty) directory skips straight to an empty Phase 1
// (nothing to recover) and an empty Phase 2 (no objects yet) -- the
// Catalog constructor already creates the directory itself (catalog.h).
//
// Concurrency: `CreateTable`/`CreateIndex` mutate `objects_` (adding a
// newly-created object's storage) with no mutex protecting that map,
// exactly like Catalog::Persist() has no mutex of its own (D-033) -- both
// rely on the same "no concurrent DDL" non-goal (docs/SPEC.md section 4):
// a DDL statement is only ever issued while
// `TransactionManager::HasActiveTransaction()` is false (enforced by the
// executor, task #43, docs/DECISIONS.md D-048 -- an earlier draft of this
// comment said "task #44"; that was written before this phase's task
// breakdown had settled, and the check actually lives at the top of the
// executor's CREATE TABLE/CREATE INDEX handling, not in the later
// BEGIN/COMMIT/ROLLBACK wiring), so a `CreateTable`/`CreateIndex` call
// never runs concurrently with any other thread's `GetHeapFile`/
// `GetIndex`/transactional access to `objects_`. This is the same
// precondition D-033 and D-035 already lean on, applied to one more piece
// of shared state.
class Database {
 public:
    // Opens `dir_path` if it already holds a FlintDB database (creating
    // and recovering every table/index exactly as described above), or
    // initializes a brand-new, empty one if it doesn't exist yet (the
    // Catalog constructor creates the directory; see catalog.h). Throws
    // whatever Catalog/LogManager/RunRecovery throw on a malformed or
    // corrupted database -- this project does not attempt automatic
    // repair of a database that fails to open (consistent with Catalog's
    // own policy, see catalog.h).
    explicit Database(std::string dir_path);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Creates a new table (Catalog::CreateTable -- see catalog.h for the
    // validation and implicit-PK-index rules) and immediately brings its
    // storage online: a fresh DiskManager+BufferPool+HeapFile for the
    // table itself, and -- if `primary_key_column` is set -- a second
    // fresh DiskManager+BufferPool+BPlusTree for its implicit unique PK
    // index (D-040), both registered with the TransactionManager. Neither
    // needs recovery: a just-allocated ObjectId (Catalog::AllocateObjectId
    // is a monotonic counter that never reuses an id) can never have any
    // existing WAL record naming it.
    ObjectId CreateTable(const std::string& name, std::vector<ColumnDef> columns,
                         std::optional<std::string> primary_key_column);

    // Creates a new secondary index (Catalog::CreateIndex) and brings its
    // storage online the same way -- a fresh DiskManager+BufferPool+
    // BPlusTree, registered with the TransactionManager.
    //
    // If `populate` is given, it's called with the new (empty) BPlusTree
    // before this method returns -- the hook the executor (task #43)
    // uses to backfill an index created on a table that already has rows
    // (docs/SPEC.md's CREATE INDEX has no notion of an index that only
    // covers rows inserted after it existed). Database itself has no
    // idea how to decode a HeapFile row's bytes into a column value (that
    // knowledge -- row_codec.h -- belongs to the SQL layer above it), so
    // it can't backfill on its own; `populate` is how the executor
    // supplies that logic while still letting Database own what happens
    // around it. After `populate` returns, every page it may have
    // dirtied is force-flushed (BufferPool::FlushAll on this index's own,
    // brand-new pool) -- the same reasoning as the BPlusTree
    // constructor's own initial-header force-flush (docs/DECISIONS.md
    // D-043): a backfill runs as DDL, never inside a transaction (SPEC
    // section 4: no concurrent DDL), so nothing else will ever flush
    // these pages if this method doesn't -- see D-048 for the full
    // reasoning. FlushAll (not FlushPages over a tracked subset) is safe
    // here specifically because this BufferPool backs a brand-new file
    // nothing else has ever touched, unlike TransactionManager::Commit's
    // own FlushAll-vs-FlushPages distinction for a long-lived,
    // concurrently-used pool.
    ObjectId CreateIndex(const std::string& name, const std::string& table_name, const std::string& column_name,
                         const std::function<void(BPlusTree&)>& populate = {});

    // Schema introspection, for the planner/executor (tasks #42/#43) to
    // build on -- Database never duplicates what Catalog already answers.
    const Catalog& GetCatalog() const { return catalog_; }

    // The one shared TransactionManager spanning every object in this
    // Database -- the executor (task #43) and BEGIN/COMMIT/ROLLBACK
    // wiring (task #44) call Begin/Commit/Abort on this directly, exactly
    // as every Phase 4 test already does against a standalone
    // TransactionManager.
    TransactionManager& GetTransactionManager() { return txn_manager_; }

    // Direct storage access, by ObjectId. Throws std::out_of_range if
    // `object_id` doesn't name a currently-open object in this Database --
    // a caller is only ever expected to pass an id it already got from
    // this Database's own Catalog (via GetCatalog()), so a miss here
    // means a real caller bug, not a normal "not found," which is why
    // this fails loud via `.at()` rather than returning nullptr (mirrors
    // RunRecovery's own `.at()` choice, wal/recovery.h).
    HeapFile* GetHeapFile(ObjectId object_id);
    BPlusTree* GetIndex(ObjectId object_id);

    // Convenience by-name lookups layering Catalog::FindTable/FindIndex
    // underneath -- return nullptr if `name` doesn't name an existing
    // table/index at all (a normal, expected outcome the executor needs
    // to turn into a SQL-level "no such table" error, not an internal
    // invariant violation), but still fail loud via GetHeapFile/GetIndex's
    // `.at()` if the name resolves to an ObjectId this Database
    // somehow doesn't have storage open for (that would be the real bug).
    HeapFile* GetHeapFileForTable(const std::string& table_name);
    BPlusTree* GetIndexByName(const std::string& index_name);

 private:
    // One table's or index's full storage stack. Exactly one of
    // heap_file/btree is non-null for any given entry -- an ObjectId
    // names either a table's heap or an index's tree, never both (see
    // Catalog::TableInfo::heap_object_id vs IndexInfo::object_id, which
    // are drawn from the same counter but never overlap) -- kept as two
    // plain nullable fields rather than a std::variant to match this
    // project's existing preference for plain, explicit fields over
    // cleverness (e.g. TableInfo::primary_key_column, catalog.h).
    struct ObjectStorage {
        std::unique_ptr<DiskManager> disk_manager;
        std::unique_ptr<BufferPool> buffer_pool;
        std::unique_ptr<HeapFile> heap_file;
        std::unique_ptr<BPlusTree> btree;
    };

    // Phase 2 of opening (see the class comment): wraps an
    // already-recovered `disk_manager` in a fresh BufferPool, builds a
    // HeapFile on top, stores it in objects_, and registers it with
    // txn_manager_.
    void AttachHeapStorage(ObjectId object_id, std::unique_ptr<DiskManager> disk_manager);

    // Same as AttachHeapStorage, but builds a BPlusTree instead -- used
    // for both a reopened index (Phase 2 of opening) and a brand-new one
    // (CreateIndex, and CreateTable's implicit PK index), since BPlusTree's
    // own constructor already handles both cases correctly (D-036).
    void AttachIndexStorage(ObjectId object_id, std::unique_ptr<DiskManager> disk_manager);

    std::string dir_path_;
    Catalog catalog_;
    LogManager log_manager_;
    TransactionManager txn_manager_;
    std::unordered_map<ObjectId, ObjectStorage> objects_;
};

}  // namespace flintdb
