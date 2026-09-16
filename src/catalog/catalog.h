#pragma once
#include "../common/config.h"

#include <optional>
#include <string>
#include <vector>

namespace flintdb {

// The two column types this engine supports (docs/SPEC.md section 1.1).
// No NULL, no REAL/BLOB/DATE/BOOLEAN -- see SPEC for why.
enum class ColumnType {
    kInteger,
    kText,
};

std::string ColumnTypeToString(ColumnType type);
ColumnType ColumnTypeFromString(const std::string& s);  // throws std::invalid_argument if unrecognized

struct ColumnDef {
    std::string name;
    ColumnType type;
};

// One index -- either an ordinary secondary index (CREATE INDEX) or the
// automatically-created index behind a table's PRIMARY KEY (see
// docs/DECISIONS.md D-040 for why a PRIMARY KEY is implemented as a
// unique index over an ordinary HeapFile, not physical row clustering).
// Either way this is a BPlusTree over `column_name`, storing `RID`s (the
// values live in `table_name`'s own HeapFile) -- see docs/SPEC.md
// section 1.3.
struct IndexInfo {
    std::string name;         // catalog-visible name; a PK index gets a synthetic one, see Catalog::CreateTable
    std::string table_name;
    std::string column_name;
    ObjectId object_id;       // this index's own BPlusTree file, see Catalog::FilePathFor
    bool is_primary_key = false;
};

// One table: its schema, which column (if any) is its PRIMARY KEY, and
// the ObjectId of the HeapFile that stores its rows. A table's indexes
// (including its PRIMARY KEY's index, if it has one) are looked up
// separately via Catalog::IndexesOnTable -- TableInfo itself only
// records the PK *column name*, not the index, so that "does this table
// have a PK" and "what BPlusTree backs it" stay two separable questions
// (the same asymmetry RID/PageKey keep between identity and location).
struct TableInfo {
    std::string name;
    std::vector<ColumnDef> columns;
    std::optional<std::string> primary_key_column;
    ObjectId heap_object_id;

    // Returns a pointer to `name`'s ColumnDef within `columns`, or
    // nullptr if this table has no such column. Every caller that needs
    // a column's type/position (executor, planner) goes through this
    // rather than re-implementing the linear search.
    const ColumnDef* FindColumn(const std::string& column_name) const;
};

// Persistent schema storage for a Phase 5 Database: table/index
// definitions, each object's assigned ObjectId, and the next ObjectId to
// hand out. Lives in one small metadata file inside the database
// directory, rewritten in full (temp file + fsync + rename) on every
// CREATE TABLE / CREATE INDEX -- never routed through the WAL. See
// docs/DECISIONS.md D-033 for why that's safe: SPEC's own non-goal ("no
// concurrent DDL -- schema changes require exclusive access to the whole
// database", docs/SPEC.md section 4) means a DDL statement's catalog
// rewrite never has to interleave with a concurrent transaction, so it
// doesn't need 2PL or WAL machinery -- a plain atomic file replace is
// sufficient, the same durability pattern countless config files use.
//
// This class only tracks *schema* -- it never opens a DiskManager/
// BufferPool/HeapFile/BPlusTree itself (that's Database's job, since only
// Database knows how to wire an ObjectId's file into the shared
// LockManager/LogManager/TransactionManager -- docs/DECISIONS.md D-034).
// FilePathFor is the one bridge between the two: given an ObjectId, it
// deterministically names that object's file on disk, so Catalog never
// has to separately persist a path string per object.
class Catalog {
 public:
    // Opens `dir_path` (creating it if it doesn't exist) and loads its
    // catalog metadata file if one is already there, or initializes a
    // brand-new, empty catalog (next_object_id_ = 0) if not. Throws
    // std::runtime_error if the directory can't be created/accessed, or
    // if an existing metadata file is present but doesn't parse (a
    // malformed or corrupted catalog is not something this project tries
    // to recover from automatically -- see the class comment on why a
    // DDL-quiescence-backed atomic rewrite is expected to never leave a
    // torn file in the first place).
    explicit Catalog(std::string dir_path);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    const std::vector<TableInfo>& Tables() const { return tables_; }
    const std::vector<IndexInfo>& Indexes() const { return indexes_; }

    // nullptr if no table/index by that name exists. Lookups are
    // case-sensitive, exact-string matches -- this project's lexer
    // (Phase 5, upcoming) does not fold identifier case, so neither does
    // this.
    const TableInfo* FindTable(const std::string& name) const;
    const IndexInfo* FindIndex(const std::string& name) const;

    // Every index (PK or secondary) defined on `table_name`, in creation
    // order. Empty if the table has no indexes at all (including no
    // PRIMARY KEY).
    std::vector<const IndexInfo*> IndexesOnTable(const std::string& table_name) const;

    // The PRIMARY KEY index for `table_name`, or nullptr if that table
    // has no PRIMARY KEY (including if the table itself doesn't exist).
    // A convenience wrapper over IndexesOnTable for the one-and-only
    // index it's ever correct to have `is_primary_key` set.
    const IndexInfo* PrimaryKeyIndex(const std::string& table_name) const;

    // Creates a new table with `columns` (must be non-empty, with
    // distinct names) and, if `primary_key_column` is set, an implicit
    // unique index on that column (docs/DECISIONS.md D-040) named
    // `"<table_name>.<primary_key_column>#pk"` -- a name no CREATE INDEX
    // statement can ever produce (column_ref syntax forbids '#' and this
    // project's lexer, built next, will reject it in an identifier),
    // guaranteeing it never collides with a user-chosen index name.
    // Allocates one ObjectId for the table's HeapFile (two, if a primary
    // key is given -- the second for its index), persists the catalog,
    // and returns the new TableInfo's heap_object_id.
    //
    // Throws std::invalid_argument if: `name` is already a table or
    // index name, `columns` is empty, two columns share a name,
    // `primary_key_column` (if set) doesn't name one of `columns`, or
    // (docs/DECISIONS.md D-046) `primary_key_column` names a TEXT column
    // -- the PRIMARY KEY's implicit index (D-040) is a BPlusTree, which
    // only supports INTEGER (int64_t) keys.
    ObjectId CreateTable(const std::string& name, std::vector<ColumnDef> columns,
                         std::optional<std::string> primary_key_column);

    // Creates a new secondary index named `name` on `table_name`'s
    // `column_name`. Allocates one ObjectId for its BPlusTree, persists
    // the catalog, and returns that ObjectId.
    //
    // Throws std::invalid_argument if: `name` is already a table or
    // index name, `table_name` doesn't name an existing table,
    // `column_name` doesn't name one of that table's columns, or
    // (docs/DECISIONS.md D-046) `column_name` is a TEXT column -- same
    // BPlusTree INTEGER-only-key restriction as CreateTable's PRIMARY KEY
    // above.
    ObjectId CreateIndex(const std::string& name, const std::string& table_name, const std::string& column_name);

    // Deterministic per-object file path: every object (a table's
    // HeapFile, or any index's BPlusTree, PK or secondary) gets its own
    // file at `<dir_path>/obj_<object_id>.dat` (docs/DECISIONS.md D-031)
    // -- this is why Catalog never needs to separately persist a path
    // string per object; the ObjectId alone determines it.
    std::string FilePathFor(ObjectId object_id) const;

    // The catalog metadata file's own path, exposed for tests that want
    // to inspect/corrupt it directly (mirroring how tests/test_log_manager.cpp
    // and tests/test_recovery.cpp poke at on-disk bytes to test torn-write
    // handling).
    std::string MetadataFilePath() const;

 private:
    void Load();     // parses MetadataFilePath() into tables_/indexes_/next_object_id_
    void Persist();  // atomic rewrite: write a temp file, fsync, rename over MetadataFilePath()

    ObjectId AllocateObjectId() { return next_object_id_++; }

    std::string dir_path_;
    std::vector<TableInfo> tables_;
    std::vector<IndexInfo> indexes_;
    ObjectId next_object_id_ = 0;
};

}  // namespace flintdb
