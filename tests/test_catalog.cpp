#include "../src/catalog/catalog.h"
#include "test_framework.h"
#include "test_utils.h"

#include <fstream>
#include <set>

using namespace flintdb;
using flintdb::testing::TempDir;

FLINTDB_TEST(catalog_opening_a_fresh_directory_creates_it_and_starts_empty) {
    TempDir dir;
    Catalog cat(dir.path());
    FLINTDB_CHECK(cat.Tables().empty());
    FLINTDB_CHECK(cat.Indexes().empty());
    FLINTDB_CHECK(cat.FindTable("anything") == nullptr);
}

FLINTDB_TEST(catalog_create_table_without_primary_key_assigns_one_object_id_and_no_index) {
    TempDir dir;
    Catalog cat(dir.path());
    ObjectId heap_id = cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}},
                                        std::nullopt);

    const TableInfo* table = cat.FindTable("widgets");
    FLINTDB_CHECK(table != nullptr);
    FLINTDB_CHECK_EQ(table->heap_object_id, heap_id);
    FLINTDB_CHECK_EQ(table->columns.size(), 2u);
    FLINTDB_CHECK(!table->primary_key_column.has_value());
    FLINTDB_CHECK(cat.IndexesOnTable("widgets").empty());
    FLINTDB_CHECK(cat.PrimaryKeyIndex("widgets") == nullptr);
}

FLINTDB_TEST(catalog_create_table_with_primary_key_also_creates_a_unique_pk_index) {
    // docs/DECISIONS.md D-040: a PRIMARY KEY is an implicit index, created
    // in the same CreateTable call, not a separate CREATE INDEX.
    TempDir dir;
    Catalog cat(dir.path());
    ObjectId heap_id = cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    const TableInfo* table = cat.FindTable("widgets");
    FLINTDB_CHECK(table != nullptr);
    FLINTDB_CHECK_EQ(table->heap_object_id, heap_id);
    FLINTDB_CHECK(table->primary_key_column.has_value());
    FLINTDB_CHECK_EQ(*table->primary_key_column, std::string("id"));

    const IndexInfo* pk = cat.PrimaryKeyIndex("widgets");
    FLINTDB_CHECK(pk != nullptr);
    FLINTDB_CHECK(pk->is_primary_key);
    FLINTDB_CHECK_EQ(pk->table_name, std::string("widgets"));
    FLINTDB_CHECK_EQ(pk->column_name, std::string("id"));
    FLINTDB_CHECK(pk->object_id != heap_id);  // its own, separate file -- see D-031

    auto all_indexes = cat.IndexesOnTable("widgets");
    FLINTDB_CHECK_EQ(all_indexes.size(), 1u);
    FLINTDB_CHECK(all_indexes[0] == pk);
}

FLINTDB_TEST(catalog_create_table_rejects_empty_columns_duplicate_names_and_bad_primary_key) {
    TempDir dir;
    Catalog cat(dir.path());

    bool threw = false;
    try {
        cat.CreateTable("t1", {}, std::nullopt);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    threw = false;
    try {
        cat.CreateTable("t2", {{"a", ColumnType::kInteger}, {"a", ColumnType::kText}}, std::nullopt);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    threw = false;
    try {
        cat.CreateTable("t3", {{"a", ColumnType::kInteger}}, "not_a_column");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    // None of the rejected attempts should have left partial state behind.
    FLINTDB_CHECK(cat.FindTable("t1") == nullptr);
    FLINTDB_CHECK(cat.FindTable("t2") == nullptr);
    FLINTDB_CHECK(cat.FindTable("t3") == nullptr);
}

FLINTDB_TEST(catalog_create_table_rejects_a_name_already_used_by_a_table_or_index) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, std::nullopt);
    cat.CreateIndex("widgets_id_idx", "widgets", "id");

    bool threw = false;
    try {
        cat.CreateTable("widgets", {{"x", ColumnType::kInteger}}, std::nullopt);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    threw = false;
    try {
        cat.CreateTable("widgets_id_idx", {{"x", ColumnType::kInteger}}, std::nullopt);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(catalog_create_index_on_unknown_table_or_column_throws) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, std::nullopt);

    bool threw = false;
    try {
        cat.CreateIndex("bad_idx", "no_such_table", "id");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    threw = false;
    try {
        cat.CreateIndex("bad_idx2", "widgets", "no_such_column");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    FLINTDB_CHECK(cat.FindIndex("bad_idx") == nullptr);
    FLINTDB_CHECK(cat.FindIndex("bad_idx2") == nullptr);
}

// docs/DECISIONS.md D-046: BPlusTree only supports int64_t keys, so
// neither a PRIMARY KEY nor a secondary index can be built on a TEXT
// column until a byte-comparable TEXT encoding exists (SPEC section 6,
// open question 5) -- both must be rejected at CREATE time, not silently
// accepted and left broken.
FLINTDB_TEST(catalog_create_table_rejects_a_text_primary_key_column_D046) {
    TempDir dir;
    Catalog cat(dir.path());

    bool threw = false;
    try {
        cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "name");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);

    // The failed CreateTable must not have left a half-created table
    // behind -- CreateTable validates the primary key column before
    // pushing anything into tables_/indexes_ or allocating any ObjectId.
    FLINTDB_CHECK(cat.FindTable("widgets") == nullptr);
}

FLINTDB_TEST(catalog_create_index_rejects_a_text_column_D046) {
    TempDir dir;
    Catalog cat(dir.path());
    cat.CreateTable("widgets", {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}}, "id");

    bool threw = false;
    try {
        cat.CreateIndex("widgets_name_idx", "widgets", "name");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
    FLINTDB_CHECK(cat.FindIndex("widgets_name_idx") == nullptr);

    // A subsequent INTEGER-column index must still work fine -- the
    // rejection above must not have left the catalog's next_object_id_ or
    // indexes_ in some corrupted state.
    ObjectId ok_id = cat.CreateIndex("widgets_id2_idx", "widgets", "id");
    FLINTDB_CHECK(cat.FindIndex("widgets_id2_idx") != nullptr);
    FLINTDB_CHECK(cat.FindIndex("widgets_id2_idx")->object_id == ok_id);
}

FLINTDB_TEST(catalog_multiple_secondary_indexes_on_the_same_table_all_show_up_via_indexes_on_table) {
    TempDir dir;
    Catalog cat(dir.path());
    // Both secondary indexes below are on INTEGER columns -- D-046
    // restricts CreateIndex to INTEGER columns only (BPlusTree only
    // supports int64_t keys), so "name" (TEXT) can't be one of them.
    cat.CreateTable("widgets",
                     {{"id", ColumnType::kInteger},
                      {"name", ColumnType::kText},
                      {"qty", ColumnType::kInteger},
                      {"weight", ColumnType::kInteger}},
                     "id");
    cat.CreateIndex("widgets_weight_idx", "widgets", "weight");
    cat.CreateIndex("widgets_qty_idx", "widgets", "qty");

    auto indexes = cat.IndexesOnTable("widgets");
    FLINTDB_CHECK_EQ(indexes.size(), 3u);  // PK index + two secondary
    int pk_count = 0;
    for (auto* idx : indexes) {
        if (idx->is_primary_key) pk_count++;
    }
    FLINTDB_CHECK_EQ(pk_count, 1);
}

FLINTDB_TEST(catalog_object_ids_are_unique_and_monotonically_increasing_across_tables_and_indexes) {
    TempDir dir;
    Catalog cat(dir.path());
    ObjectId a = cat.CreateTable("t_a", {{"id", ColumnType::kInteger}}, "id");  // allocates 2 ids (heap + pk index)
    ObjectId b = cat.CreateTable("t_b", {{"id", ColumnType::kInteger}}, std::nullopt);  // allocates 1 id
    ObjectId c = cat.CreateIndex("extra_idx", "t_b", "id");

    const IndexInfo* pk_a = cat.PrimaryKeyIndex("t_a");
    FLINTDB_CHECK(pk_a != nullptr);

    std::set<ObjectId> seen = {a, pk_a->object_id, b, c};
    FLINTDB_CHECK_EQ(seen.size(), 4u);  // no collisions
    FLINTDB_CHECK(a < pk_a->object_id);
    FLINTDB_CHECK(pk_a->object_id < b);
    FLINTDB_CHECK(b < c);
}

FLINTDB_TEST(catalog_file_path_for_is_deterministic_and_distinct_per_object) {
    TempDir dir;
    Catalog cat(dir.path());
    FLINTDB_CHECK(cat.FilePathFor(0) != cat.FilePathFor(1));
    FLINTDB_CHECK_EQ(cat.FilePathFor(5), cat.FilePathFor(5));  // pure function of the id, not stateful
}

FLINTDB_TEST(catalog_persists_schema_across_reopen_including_columns_primary_key_and_indexes) {
    TempDir dir;
    ObjectId heap_id, pk_object_id, sec_object_id;
    {
        Catalog cat(dir.path());
        heap_id =
            cat.CreateTable("widgets",
                             {{"id", ColumnType::kInteger}, {"name", ColumnType::kText}, {"qty", ColumnType::kInteger}},
                             "id");
        pk_object_id = cat.PrimaryKeyIndex("widgets")->object_id;
        // D-046: secondary indexes are INTEGER-only (BPlusTree only
        // supports int64_t keys) -- "qty", not "name", is the one indexed
        // here; "name" (TEXT) still round-trips as an ordinary column,
        // checked below.
        sec_object_id = cat.CreateIndex("widgets_qty_idx", "widgets", "qty");
    }  // Catalog destructed -- nothing kept in memory; only what Persist() wrote survives

    Catalog reopened(dir.path());
    const TableInfo* table = reopened.FindTable("widgets");
    FLINTDB_CHECK(table != nullptr);
    FLINTDB_CHECK_EQ(table->heap_object_id, heap_id);
    FLINTDB_CHECK_EQ(table->columns.size(), 3u);
    FLINTDB_CHECK_EQ(table->columns[0].name, std::string("id"));
    FLINTDB_CHECK(table->columns[0].type == ColumnType::kInteger);
    FLINTDB_CHECK_EQ(table->columns[1].name, std::string("name"));
    FLINTDB_CHECK(table->columns[1].type == ColumnType::kText);
    FLINTDB_CHECK_EQ(table->columns[2].name, std::string("qty"));
    FLINTDB_CHECK(table->columns[2].type == ColumnType::kInteger);
    FLINTDB_CHECK(table->primary_key_column.has_value());
    FLINTDB_CHECK_EQ(*table->primary_key_column, std::string("id"));

    const IndexInfo* pk = reopened.PrimaryKeyIndex("widgets");
    FLINTDB_CHECK(pk != nullptr);
    FLINTDB_CHECK_EQ(pk->object_id, pk_object_id);

    const IndexInfo* sec = reopened.FindIndex("widgets_qty_idx");
    FLINTDB_CHECK(sec != nullptr);
    FLINTDB_CHECK_EQ(sec->object_id, sec_object_id);
    FLINTDB_CHECK(!sec->is_primary_key);

    // And the next ObjectId handed out after reopening must not collide
    // with anything already on record -- proves next_object_id_ itself
    // round-tripped, not just the table/index rows.
    ObjectId new_id = reopened.CreateTable("gadgets", {{"x", ColumnType::kInteger}}, std::nullopt);
    FLINTDB_CHECK(new_id != heap_id);
    FLINTDB_CHECK(new_id != pk_object_id);
    FLINTDB_CHECK(new_id != sec_object_id);
}

FLINTDB_TEST(catalog_reopening_an_empty_but_already_created_directory_stays_empty) {
    TempDir dir;
    {
        Catalog cat(dir.path());  // creates the directory, but no tables/indexes -- no metadata file written yet
    }
    Catalog reopened(dir.path());
    FLINTDB_CHECK(reopened.Tables().empty());
    FLINTDB_CHECK(reopened.Indexes().empty());
}

FLINTDB_TEST(catalog_metadata_file_survives_a_torn_write_of_a_later_update_because_of_atomic_rename) {
    // This is the actual safety property D-033's atomic-rewrite design
    // provides: a crash *during* a later Persist() call must never
    // corrupt the previously-durable catalog, because the rewrite always
    // happens into a separate temp file first. Simulate that directly by
    // writing garbage into the ".tmp" path a real Persist() would use,
    // without ever renaming it over the real file -- exactly what a crash
    // mid-rewrite leaves behind -- then confirm the original catalog
    // (from before that crash) is completely unaffected on reopen.
    TempDir dir;
    ObjectId heap_id;
    {
        Catalog cat(dir.path());
        heap_id = cat.CreateTable("widgets", {{"id", ColumnType::kInteger}}, std::nullopt);
    }

    std::string tmp_path = dir.path() + "/catalog.meta.tmp";
    {
        std::ofstream garbage(tmp_path);
        garbage << "this is not a valid catalog file";
    }

    Catalog reopened(dir.path());  // must load the *real* catalog.meta, never even look at the .tmp file
    const TableInfo* table = reopened.FindTable("widgets");
    FLINTDB_CHECK(table != nullptr);
    FLINTDB_CHECK_EQ(table->heap_object_id, heap_id);
}
