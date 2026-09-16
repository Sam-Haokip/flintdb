#include "catalog.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace flintdb {

std::string ColumnTypeToString(ColumnType type) {
    switch (type) {
        case ColumnType::kInteger:
            return "INTEGER";
        case ColumnType::kText:
            return "TEXT";
    }
    throw std::invalid_argument("ColumnTypeToString: unrecognized ColumnType");
}

ColumnType ColumnTypeFromString(const std::string& s) {
    if (s == "INTEGER") return ColumnType::kInteger;
    if (s == "TEXT") return ColumnType::kText;
    throw std::invalid_argument("ColumnTypeFromString: unrecognized column type '" + s + "'");
}

const ColumnDef* TableInfo::FindColumn(const std::string& column_name) const {
    for (const ColumnDef& c : columns) {
        if (c.name == column_name) return &c;
    }
    return nullptr;
}

namespace {

// Same "loop until every byte is written" discipline as
// wal/log_manager.cpp's FullWrite -- write() is permitted to transfer
// fewer bytes than requested.
void FullWrite(int fd, const std::string& data) {
    const char* p = data.data();
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(fd, p + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("Catalog: write failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
}

// Reads one whitespace-delimited token from `in` and throws if it isn't
// exactly `expected` -- the parser's only error-reporting mechanism,
// used for every fixed tag in the format (TABLE, COLUMN, ...) so a
// malformed catalog file fails loudly with a specific complaint rather
// than silently misparsing.
void ExpectTag(std::istream& in, const std::string& expected) {
    std::string tag;
    if (!(in >> tag) || tag != expected) {
        throw std::runtime_error("Catalog: malformed metadata file (expected '" + expected + "', got '" + tag + "')");
    }
}

template <typename T>
T ReadField(std::istream& in, const std::string& tag) {
    ExpectTag(in, tag);
    T value;
    if (!(in >> value)) {
        throw std::runtime_error("Catalog: malformed metadata file (bad value after '" + tag + "')");
    }
    return value;
}

}  // namespace

Catalog::Catalog(std::string dir_path) : dir_path_(std::move(dir_path)) {
    // Create the database directory if this is a brand-new database.
    // EEXIST is expected and fine (reopening an existing one); anything
    // else is a real failure to report.
    if (mkdir(dir_path_.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("Catalog: failed to create directory " + dir_path_ + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (stat(MetadataFilePath().c_str(), &st) == 0) {
        Load();
    }
    // Otherwise: no metadata file yet -- a fresh, empty catalog (the
    // member defaults: empty tables_/indexes_, next_object_id_ = 0) is
    // already the correct starting state, and the first CreateTable/
    // CreateIndex call will persist it into existence.
}

void Catalog::Load() {
    std::ifstream in(MetadataFilePath());
    if (!in) {
        throw std::runtime_error("Catalog: failed to open " + MetadataFilePath() + " for reading");
    }

    ExpectTag(in, "FLINTDB_CATALOG");
    int version = 0;
    if (!(in >> version) || version != 1) {
        throw std::runtime_error("Catalog: unsupported or missing catalog format version in " + MetadataFilePath());
    }

    next_object_id_ = ReadField<ObjectId>(in, "NEXT_OBJECT_ID");
    size_t num_tables = ReadField<size_t>(in, "NUM_TABLES");

    std::vector<TableInfo> tables;
    std::vector<IndexInfo> indexes;
    tables.reserve(num_tables);

    for (size_t i = 0; i < num_tables; i++) {
        TableInfo table;
        ExpectTag(in, "TABLE");
        if (!(in >> table.name)) throw std::runtime_error("Catalog: malformed metadata file (missing table name)");
        table.heap_object_id = ReadField<ObjectId>(in, "HEAP_OBJECT_ID");

        size_t num_columns = ReadField<size_t>(in, "NUM_COLUMNS");
        table.columns.reserve(num_columns);
        for (size_t c = 0; c < num_columns; c++) {
            ExpectTag(in, "COLUMN");
            std::string col_name, col_type;
            if (!(in >> col_name >> col_type)) {
                throw std::runtime_error("Catalog: malformed metadata file (bad COLUMN line)");
            }
            table.columns.push_back(ColumnDef{col_name, ColumnTypeFromString(col_type)});
        }

        ExpectTag(in, "PRIMARY_KEY");
        std::string pk;
        if (!(in >> pk)) throw std::runtime_error("Catalog: malformed metadata file (missing PRIMARY_KEY value)");
        if (pk != "NONE") table.primary_key_column = pk;

        size_t num_indexes = ReadField<size_t>(in, "NUM_INDEXES");
        for (size_t idx_i = 0; idx_i < num_indexes; idx_i++) {
            ExpectTag(in, "INDEX");
            IndexInfo idx;
            std::string kind;
            if (!(in >> idx.name >> idx.column_name >> idx.object_id >> kind)) {
                throw std::runtime_error("Catalog: malformed metadata file (bad INDEX line)");
            }
            if (kind != "PK" && kind != "SEC") {
                throw std::runtime_error("Catalog: malformed metadata file (bad index kind '" + kind + "')");
            }
            idx.table_name = table.name;
            idx.is_primary_key = (kind == "PK");
            indexes.push_back(std::move(idx));
        }

        tables.push_back(std::move(table));
    }

    tables_ = std::move(tables);
    indexes_ = std::move(indexes);
}

void Catalog::Persist() {
    std::ostringstream oss;
    oss << "FLINTDB_CATALOG 1\n";
    oss << "NEXT_OBJECT_ID " << next_object_id_ << "\n";
    oss << "NUM_TABLES " << tables_.size() << "\n";
    for (const TableInfo& table : tables_) {
        oss << "TABLE " << table.name << "\n";
        oss << "HEAP_OBJECT_ID " << table.heap_object_id << "\n";
        oss << "NUM_COLUMNS " << table.columns.size() << "\n";
        for (const ColumnDef& col : table.columns) {
            oss << "COLUMN " << col.name << " " << ColumnTypeToString(col.type) << "\n";
        }
        oss << "PRIMARY_KEY " << (table.primary_key_column.has_value() ? *table.primary_key_column : "NONE") << "\n";

        std::vector<const IndexInfo*> table_indexes = IndexesOnTable(table.name);
        oss << "NUM_INDEXES " << table_indexes.size() << "\n";
        for (const IndexInfo* idx : table_indexes) {
            oss << "INDEX " << idx->name << " " << idx->column_name << " " << idx->object_id << " "
                << (idx->is_primary_key ? "PK" : "SEC") << "\n";
        }
    }

    // Atomic rewrite: write the full new content to a temp file, fsync
    // it durable, then rename over the real metadata file -- rename(2)
    // is atomic on the filesystems this project targets, so a crash at
    // any point before the rename leaves the old catalog file completely
    // untouched and valid, and a crash after it leaves the new one fully
    // in place; there is no window where a reader could see a partially-
    // written catalog. Safe without any locking because of the DDL-
    // quiescence precondition this whole design leans on (see the class
    // comment and docs/DECISIONS.md D-033): nothing else is ever
    // concurrently reading or writing this file while this runs.
    std::string tmp_path = MetadataFilePath() + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Catalog: failed to open " + tmp_path + " for writing: " + std::strerror(errno));
    }
    std::string content = oss.str();
    try {
        FullWrite(fd, content);
        if (fsync(fd) != 0) {
            throw std::runtime_error(std::string("Catalog: fsync failed: ") + std::strerror(errno));
        }
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);

    if (rename(tmp_path.c_str(), MetadataFilePath().c_str()) != 0) {
        throw std::runtime_error("Catalog: rename failed: " + std::string(std::strerror(errno)));
    }
}

const TableInfo* Catalog::FindTable(const std::string& name) const {
    for (const TableInfo& table : tables_) {
        if (table.name == name) return &table;
    }
    return nullptr;
}

const IndexInfo* Catalog::FindIndex(const std::string& name) const {
    for (const IndexInfo& idx : indexes_) {
        if (idx.name == name) return &idx;
    }
    return nullptr;
}

std::vector<const IndexInfo*> Catalog::IndexesOnTable(const std::string& table_name) const {
    std::vector<const IndexInfo*> result;
    for (const IndexInfo& idx : indexes_) {
        if (idx.table_name == table_name) result.push_back(&idx);
    }
    return result;
}

const IndexInfo* Catalog::PrimaryKeyIndex(const std::string& table_name) const {
    for (const IndexInfo& idx : indexes_) {
        if (idx.table_name == table_name && idx.is_primary_key) return &idx;
    }
    return nullptr;
}

ObjectId Catalog::CreateTable(const std::string& name, std::vector<ColumnDef> columns,
                              std::optional<std::string> primary_key_column) {
    if (FindTable(name) != nullptr || FindIndex(name) != nullptr) {
        throw std::invalid_argument("Catalog::CreateTable: name '" + name + "' is already in use");
    }
    if (columns.empty()) {
        throw std::invalid_argument("Catalog::CreateTable: table '" + name + "' must have at least one column");
    }
    for (size_t i = 0; i < columns.size(); i++) {
        for (size_t j = i + 1; j < columns.size(); j++) {
            if (columns[i].name == columns[j].name) {
                throw std::invalid_argument("Catalog::CreateTable: table '" + name +
                                            "' has duplicate column name '" + columns[i].name + "'");
            }
        }
    }
    if (primary_key_column.has_value()) {
        const ColumnDef* pk_col = nullptr;
        for (const ColumnDef& col : columns) {
            if (col.name == *primary_key_column) {
                pk_col = &col;
                break;
            }
        }
        if (pk_col == nullptr) {
            throw std::invalid_argument("Catalog::CreateTable: primary key column '" + *primary_key_column +
                                        "' is not a column of table '" + name + "'");
        }
        // D-046: a PRIMARY KEY is implemented as a unique B+-tree index
        // over the heap file (D-040), and BPlusTree (index/btree.h) only
        // ever stores int64_t keys -- there is no byte-comparable TEXT
        // encoding defined yet (docs/SPEC.md section 6, open question 5).
        // Rejecting a TEXT primary key here, rather than letting one exist
        // and then fail (or silently misencode) the moment a row is
        // inserted, keeps every PRIMARY KEY this Catalog ever creates
        // actually usable.
        if (pk_col->type != ColumnType::kInteger) {
            throw std::invalid_argument("Catalog::CreateTable: primary key column '" + *primary_key_column +
                                        "' is " + ColumnTypeToString(pk_col->type) +
                                        ", but this engine's PRIMARY KEY index only supports INTEGER columns "
                                        "(see docs/DECISIONS.md D-046)");
        }
    }

    TableInfo table;
    table.name = name;
    table.columns = std::move(columns);
    table.primary_key_column = primary_key_column;
    table.heap_object_id = AllocateObjectId();
    ObjectId heap_object_id = table.heap_object_id;
    tables_.push_back(std::move(table));

    if (primary_key_column.has_value()) {
        // '#' can never appear in a user-supplied identifier (this
        // project's lexer, built next, only accepts identifier
        // characters -- letters, digits, underscore), so this synthetic
        // name can never collide with a real CREATE INDEX name.
        IndexInfo idx;
        idx.name = name + "." + *primary_key_column + "#pk";
        idx.table_name = name;
        idx.column_name = *primary_key_column;
        idx.object_id = AllocateObjectId();
        idx.is_primary_key = true;
        indexes_.push_back(std::move(idx));
    }

    Persist();
    return heap_object_id;
}

ObjectId Catalog::CreateIndex(const std::string& name, const std::string& table_name, const std::string& column_name) {
    if (FindTable(name) != nullptr || FindIndex(name) != nullptr) {
        throw std::invalid_argument("Catalog::CreateIndex: name '" + name + "' is already in use");
    }
    const TableInfo* table = FindTable(table_name);
    if (table == nullptr) {
        throw std::invalid_argument("Catalog::CreateIndex: no such table '" + table_name + "'");
    }
    const ColumnDef* column = table->FindColumn(column_name);
    if (column == nullptr) {
        throw std::invalid_argument("Catalog::CreateIndex: table '" + table_name + "' has no column '" +
                                    column_name + "'");
    }
    // D-046: same restriction as CreateTable's PRIMARY KEY column above --
    // BPlusTree only supports int64_t keys, so a secondary index can only
    // ever be built on an INTEGER column until a byte-comparable TEXT
    // encoding is designed (docs/SPEC.md section 6, open question 5).
    if (column->type != ColumnType::kInteger) {
        throw std::invalid_argument("Catalog::CreateIndex: column '" + column_name + "' is " +
                                    ColumnTypeToString(column->type) +
                                    ", but this engine's B+-tree index only supports INTEGER columns "
                                    "(see docs/DECISIONS.md D-046)");
    }

    IndexInfo idx;
    idx.name = name;
    idx.table_name = table_name;
    idx.column_name = column_name;
    idx.object_id = AllocateObjectId();
    idx.is_primary_key = false;
    ObjectId object_id = idx.object_id;
    indexes_.push_back(std::move(idx));

    Persist();
    return object_id;
}

std::string Catalog::FilePathFor(ObjectId object_id) const {
    return dir_path_ + "/obj_" + std::to_string(object_id) + ".dat";
}

std::string Catalog::MetadataFilePath() const { return dir_path_ + "/catalog.meta"; }

}  // namespace flintdb
