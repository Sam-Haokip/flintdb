#include "executor.h"

#include "planner.h"
#include "row_codec.h"
#include "sql_error.h"
#include "sql_semantics.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace flintdb {

namespace {

// ---------------------------------------------------------------------
// Value comparison -- shared by residual-predicate evaluation and by a
// sequential-scan join's explicit equality check.
// ---------------------------------------------------------------------

// Kind-agnostic equality, used by a sequential-scan JOIN's explicit
// "inner_join_column == outer's value" check (docs/DECISIONS.md D-048):
// unlike an index-scan join (D-046 guarantees the indexed, and therefore
// probed, column is always INTEGER), a sequential-scan join can legally
// be comparing two TEXT columns, so this can't just read int_value.
bool LiteralsEqual(const LiteralValue& a, const LiteralValue& b) {
    if (a.kind == LiteralValue::Kind::kInteger) return a.int_value == b.int_value;
    return a.string_value == b.string_value;
}

// -1/0/1, the way std::string::compare or an integer subtraction would --
// the one place every Comparator (kEq/kNeq/kLt/kLe/kGt/kGe) bottoms out,
// so residual-predicate evaluation below doesn't need six separate
// per-comparator branches for each of the two LiteralValue kinds.
int CompareLiterals(const LiteralValue& a, const LiteralValue& b) {
    if (a.kind == LiteralValue::Kind::kInteger) {
        if (a.int_value < b.int_value) return -1;
        if (a.int_value > b.int_value) return 1;
        return 0;
    }
    if (a.string_value < b.string_value) return -1;
    if (a.string_value > b.string_value) return 1;
    return 0;
}

bool EvaluatePredicate(const TableInfo& table, const std::vector<LiteralValue>& values, const Predicate& pred) {
    size_t idx = ColumnIndex(table, pred.column.column_name);
    int cmp = CompareLiterals(values[idx], pred.value);
    switch (pred.op) {
        case Comparator::kEq:
            return cmp == 0;
        case Comparator::kNeq:
            return cmp != 0;
        case Comparator::kLt:
            return cmp < 0;
        case Comparator::kLe:
            return cmp <= 0;
        case Comparator::kGt:
            return cmp > 0;
        case Comparator::kGe:
            return cmp >= 0;
    }
    throw std::logic_error("EvaluatePredicate: unreachable -- every Comparator is handled above");
}

// AND-conjunction (docs/SPEC.md section 1.3: WHERE is AND-only) over
// `predicates` -- vacuously true for an empty list, matching "no WHERE at
// all" and "every residual already consumed by the driving index probe."
bool MatchesAll(const TableInfo& table, const std::vector<LiteralValue>& values,
                 const std::vector<Predicate>& predicates) {
    for (const Predicate& pred : predicates) {
        if (!EvaluatePredicate(table, values, pred)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Row access -- turning a TableAccessPlan into actual (RID, row bytes).
// ---------------------------------------------------------------------

// Fetches every candidate row for `access`'s table: every row
// (kSeqScan), or just the ones matching its driving index probe
// (kIndexScan). `access.residual_predicates` is deliberately NOT applied
// here -- every caller needs the decoded values anyway (to project or to
// mutate the row), so filtering happens once, after decoding, via
// MatchesAll above.
std::vector<std::pair<RID, std::string>> FetchCandidateRows(Database& db, const TableAccessPlan& access) {
    if (access.method == ScanMethod::kSeqScan) {
        return db.GetHeapFileForTable(access.table_name)->Scan();
    }
    BPlusTree* index = db.GetIndexByName(access.index_name);
    HeapFile* heap = db.GetHeapFileForTable(access.table_name);
    std::vector<std::pair<RID, std::string>> rows;
    for (RID rid : index->Search(access.index_key)) {
        std::optional<std::string> row_bytes = heap->GetRow(rid);
        // A live index entry always names a live row -- INSERT/UPDATE/
        // DELETE below keep every index in lockstep with the heap file on
        // every mutation (docs/DECISIONS.md D-048) -- so a miss here
        // would mean real index/heap divergence, not a routine "not
        // found" a caller should handle gracefully.
        if (!row_bytes.has_value()) {
            throw std::logic_error("FetchCandidateRows: index '" + access.index_name +
                                    "' names a RID with no live row -- index and heap file are out of sync");
        }
        rows.emplace_back(rid, std::move(*row_bytes));
    }
    return rows;
}

// ---------------------------------------------------------------------
// Index maintenance and PRIMARY KEY uniqueness -- shared by INSERT,
// UPDATE (delete-then-reinsert), and DELETE.
// ---------------------------------------------------------------------

void InsertIntoAllIndexes(Database& db, const Catalog& catalog, const TableInfo& table,
                           const std::vector<LiteralValue>& values, RID rid) {
    for (const IndexInfo* idx : catalog.IndexesOnTable(table.name)) {
        size_t col_idx = ColumnIndex(table, idx->column_name);
        // D-046: every index this Catalog could ever contain is on an
        // INTEGER column -- int_value is always the right field to read.
        db.GetIndex(idx->object_id)->Insert(values[col_idx].int_value, rid);
    }
}

void DeleteFromAllIndexes(Database& db, const Catalog& catalog, const TableInfo& table,
                           const std::vector<LiteralValue>& values, RID rid) {
    for (const IndexInfo* idx : catalog.IndexesOnTable(table.name)) {
        size_t col_idx = ColumnIndex(table, idx->column_name);
        db.GetIndex(idx->object_id)->Delete(values[col_idx].int_value, rid);
    }
}

// Throws SqlSemanticError if `table` has a PRIMARY KEY and `values`'
// value at that column already exists in the PK index. Callers run this
// against an index that no longer contains the row being updated/
// inserted (i.e. before INSERT touches anything, and after UPDATE has
// already deleted the old entry) -- see docs/DECISIONS.md D-048 for why
// that ordering makes an unchanged PRIMARY KEY re-insert its own
// just-vacated key correctly, rather than spuriously colliding with
// itself.
void CheckPrimaryKeyUniqueness(Database& db, const Catalog& catalog, const TableInfo& table,
                                const std::vector<LiteralValue>& values) {
    const IndexInfo* pk = catalog.PrimaryKeyIndex(table.name);
    if (pk == nullptr) return;
    int64_t key = values[ColumnIndex(table, pk->column_name)].int_value;
    if (!db.GetIndex(pk->object_id)->Search(key).empty()) {
        throw SqlSemanticError("duplicate PRIMARY KEY value " + std::to_string(key) + " for table '" + table.name +
                                "'");
    }
}

// Throws SqlSemanticError if `values.size() != table.columns.size()`
// (docs/SPEC.md section 1.3: INSERT requires a full value list in
// column-declaration order, no partial-column inserts) or any value's
// kind doesn't match its column's declared type.
void ValidateValuesAgainstColumns(const TableInfo& table, const std::vector<LiteralValue>& values) {
    if (values.size() != table.columns.size()) {
        throw SqlSemanticError("table '" + table.name + "' has " + std::to_string(table.columns.size()) +
                                " column(s) but " + std::to_string(values.size()) + " value(s) were given");
    }
    for (size_t i = 0; i < values.size(); i++) {
        RequireLiteralMatchesColumn(table, table.columns[i], values[i]);
    }
}

// Throws std::logic_error if any transaction is active anywhere --
// docs/SPEC.md section 4 ("no concurrent DDL: schema changes require
// exclusive access to the whole database") and Database's own class
// comment (db/database.h) both depend on this holding for every CREATE
// TABLE/CREATE INDEX this executor runs, since Database::objects_ and
// Catalog::Persist() both mutate shared state with no mutex of their own.
void RequireNoActiveTransaction(Database& db, const char* statement_kind) {
    if (db.GetTransactionManager().HasActiveTransaction()) {
        throw std::logic_error(std::string("DDL statement (") + statement_kind +
                                ") cannot run while a transaction is active anywhere "
                                "(docs/SPEC.md section 4: no concurrent DDL)");
    }
}

// ---------------------------------------------------------------------
// Per-statement execution.
// ---------------------------------------------------------------------

ExecuteResult ExecuteCreateTable(Database& db, const CreateTableStatement& stmt) {
    RequireNoActiveTransaction(db, "CREATE TABLE");
    db.CreateTable(stmt.table_name, stmt.columns, stmt.primary_key_column);
    return ExecuteResult{};
}

ExecuteResult ExecuteCreateIndex(Database& db, const CreateIndexStatement& stmt) {
    RequireNoActiveTransaction(db, "CREATE INDEX");
    // docs/DECISIONS.md D-048: backfill the new index with every row the
    // table already has -- Database::CreateIndex's `populate` hook runs
    // this before returning, then force-flushes the (brand-new) index's
    // BufferPool so the backfill is durable despite running outside any
    // transaction.
    db.CreateIndex(stmt.index_name, stmt.table_name, stmt.column_name, [&db, &stmt](BPlusTree& index) {
        // Catalog::CreateIndex (called inside Database::CreateIndex, just
        // above) already validated table_name/column_name -- both are
        // guaranteed to exist by the time this callback runs.
        const TableInfo* table = db.GetCatalog().FindTable(stmt.table_name);
        size_t col_idx = ColumnIndex(*table, stmt.column_name);
        for (const auto& [rid, row_bytes] : db.GetHeapFileForTable(stmt.table_name)->Scan()) {
            std::vector<LiteralValue> values = DecodeRow(table->columns, row_bytes);
            index.Insert(values[col_idx].int_value, rid);
        }
    });
    return ExecuteResult{};
}

ExecuteResult ExecuteInsert(Database& db, const InsertStatement& stmt) {
    const Catalog& catalog = db.GetCatalog();
    const TableInfo* table = catalog.FindTable(stmt.table_name);
    if (table == nullptr) {
        throw SqlSemanticError("no such table '" + stmt.table_name + "'");
    }
    ValidateValuesAgainstColumns(*table, stmt.values);
    // Checked before any mutation -- see CheckPrimaryKeyUniqueness's own
    // comment for why a rejected INSERT never leaves a partially-applied
    // row behind.
    CheckPrimaryKeyUniqueness(db, catalog, *table, stmt.values);

    HeapFile* heap = db.GetHeapFileForTable(stmt.table_name);
    std::string row_bytes = EncodeRow(table->columns, stmt.values);
    RID rid = heap->Insert(row_bytes);
    InsertIntoAllIndexes(db, catalog, *table, stmt.values, rid);

    ExecuteResult result;
    result.rows_affected = 1;
    return result;
}

ExecuteResult ExecuteDelete(Database& db, const DeleteStatement& stmt) {
    const Catalog& catalog = db.GetCatalog();
    TableAccessPlan access = PlanTableAccess(catalog, stmt.table_name, stmt.where_predicates);
    const TableInfo* table = catalog.FindTable(stmt.table_name);  // PlanTableAccess already confirmed this exists
    HeapFile* heap = db.GetHeapFileForTable(stmt.table_name);

    ExecuteResult result;
    for (auto& [rid, row_bytes] : FetchCandidateRows(db, access)) {
        std::vector<LiteralValue> values = DecodeRow(table->columns, row_bytes);
        if (!MatchesAll(*table, values, access.residual_predicates)) continue;
        DeleteFromAllIndexes(db, catalog, *table, values, rid);
        heap->Delete(rid);
        result.rows_affected++;
    }
    return result;
}

ExecuteResult ExecuteUpdate(Database& db, const UpdateStatement& stmt) {
    const Catalog& catalog = db.GetCatalog();
    const TableInfo* table = catalog.FindTable(stmt.table_name);
    if (table == nullptr) {
        throw SqlSemanticError("no such table '" + stmt.table_name + "'");
    }

    // Validate + resolve every assignment's column once, up front, before
    // touching any row.
    std::vector<size_t> assignment_col_idx;
    assignment_col_idx.reserve(stmt.assignments.size());
    for (const Assignment& a : stmt.assignments) {
        const ColumnDef& col = RequireColumn(*table, a.column_name);
        RequireLiteralMatchesColumn(*table, col, a.value);
        assignment_col_idx.push_back(ColumnIndex(*table, a.column_name));
    }

    TableAccessPlan access = PlanTableAccess(catalog, stmt.table_name, stmt.where_predicates);
    HeapFile* heap = db.GetHeapFileForTable(stmt.table_name);

    // Fully materialize the matching (RID, old values) list *before*
    // mutating anything -- docs/DECISIONS.md D-048: this closes off the
    // Halloween problem (an UPDATE re-matching a row it already moved) by
    // construction, since this list is a fixed snapshot, not a live
    // re-query the mutations below could ever affect.
    std::vector<std::pair<RID, std::vector<LiteralValue>>> matched;
    for (auto& [rid, row_bytes] : FetchCandidateRows(db, access)) {
        std::vector<LiteralValue> values = DecodeRow(table->columns, row_bytes);
        if (!MatchesAll(*table, values, access.residual_predicates)) continue;
        matched.emplace_back(rid, std::move(values));
    }

    ExecuteResult result;
    for (auto& [rid, old_values] : matched) {
        std::vector<LiteralValue> new_values = old_values;
        for (size_t i = 0; i < stmt.assignments.size(); i++) {
            new_values[assignment_col_idx[i]] = stmt.assignments[i].value;
        }

        // HeapFile has no in-place update (storage/heap_file.h), so this
        // is delete-then-reinsert: every index -- not just ones the SET
        // clause actually touches -- gets its old (value, RID) entry
        // removed here and a new one added below, since the RID changes
        // regardless of whether a given index's own column did.
        DeleteFromAllIndexes(db, catalog, *table, old_values, rid);
        heap->Delete(rid);

        // Re-checked against an index that no longer holds this row's own
        // old PK entry -- see CheckPrimaryKeyUniqueness's comment.
        CheckPrimaryKeyUniqueness(db, catalog, *table, new_values);

        std::string new_row_bytes = EncodeRow(table->columns, new_values);
        RID new_rid = heap->Insert(new_row_bytes);
        InsertIntoAllIndexes(db, catalog, *table, new_values, new_rid);

        result.rows_affected++;
    }
    return result;
}

// ---------------------------------------------------------------------
// SELECT.
// ---------------------------------------------------------------------

void AppendColumnNames(const TableInfo& table, std::vector<std::string>& out) {
    for (const ColumnDef& col : table.columns) out.push_back(col.name);
}

ExecuteResult ExecuteSelectSingleTable(Database& db, const SelectPlan& plan, const TableAccessPlan& access) {
    const Catalog& catalog = db.GetCatalog();
    const TableInfo* table = catalog.FindTable(access.table_name);

    ExecuteResult result;
    if (plan.select_star) {
        AppendColumnNames(*table, result.column_names);
    } else {
        for (const ColumnRef& col : plan.select_list) result.column_names.push_back(col.column_name);
    }

    for (auto& [rid, row_bytes] : FetchCandidateRows(db, access)) {
        (void)rid;
        std::vector<LiteralValue> values = DecodeRow(table->columns, row_bytes);
        if (!MatchesAll(*table, values, access.residual_predicates)) continue;
        if (plan.select_star) {
            result.rows.push_back(values);
            continue;
        }
        std::vector<LiteralValue> projected;
        projected.reserve(plan.select_list.size());
        for (const ColumnRef& col : plan.select_list) {
            projected.push_back(values[ColumnIndex(*table, col.column_name)]);
        }
        result.rows.push_back(std::move(projected));
    }
    return result;
}

ExecuteResult ExecuteSelectJoin(Database& db, const SelectPlan& plan, const JoinPlan& jp) {
    const Catalog& catalog = db.GetCatalog();
    const TableInfo* outer_table = catalog.FindTable(jp.outer.table_name);
    const TableInfo* inner_table = catalog.FindTable(jp.inner_table_name);
    size_t outer_join_idx = ColumnIndex(*outer_table, jp.outer_join_column);
    size_t inner_join_idx = ColumnIndex(*inner_table, jp.inner_join_column);

    ExecuteResult result;
    if (plan.select_star) {
        AppendColumnNames(*outer_table, result.column_names);
        AppendColumnNames(*inner_table, result.column_names);
    } else {
        for (const ColumnRef& col : plan.select_list) result.column_names.push_back(col.column_name);
    }

    HeapFile* inner_heap = db.GetHeapFileForTable(jp.inner_table_name);
    BPlusTree* inner_index = (jp.inner_method == ScanMethod::kIndexScan) ? db.GetIndexByName(jp.inner_index_name)
                                                                          : nullptr;
    // For a sequential inner scan, every inner row is fetched once, up
    // front, and reused across every outer iteration -- SELECT never
    // mutates anything, so (unlike UPDATE's snapshot, which exists for
    // correctness) this is purely to avoid re-scanning the whole inner
    // table once per outer row.
    std::vector<std::pair<RID, std::string>> inner_seq_rows;
    if (jp.inner_method == ScanMethod::kSeqScan) inner_seq_rows = inner_heap->Scan();

    for (auto& [outer_rid, outer_bytes] : FetchCandidateRows(db, jp.outer)) {
        (void)outer_rid;
        std::vector<LiteralValue> outer_values = DecodeRow(outer_table->columns, outer_bytes);
        if (!MatchesAll(*outer_table, outer_values, jp.outer.residual_predicates)) continue;

        std::vector<std::pair<RID, std::string>> inner_candidates;
        if (jp.inner_method == ScanMethod::kIndexScan) {
            // D-046: an index only ever exists on an INTEGER column, and
            // the planner (D-047) already required both join columns to
            // share the same ColumnType -- int_value is safe here.
            int64_t key = outer_values[outer_join_idx].int_value;
            for (RID rid : inner_index->Search(key)) {
                std::optional<std::string> row_bytes = inner_heap->GetRow(rid);
                if (!row_bytes.has_value()) {
                    throw std::logic_error(
                        "ExecuteSelectJoin: index names a RID with no live row -- index and heap file are out of "
                        "sync");
                }
                inner_candidates.emplace_back(rid, std::move(*row_bytes));
            }
        } else {
            inner_candidates = inner_seq_rows;
        }

        for (auto& [inner_rid, inner_bytes] : inner_candidates) {
            (void)inner_rid;
            std::vector<LiteralValue> inner_values = DecodeRow(inner_table->columns, inner_bytes);
            // An index probe already guarantees this equality exactly; a
            // sequential inner scan has to check it explicitly (D-048).
            if (jp.inner_method == ScanMethod::kSeqScan &&
                !LiteralsEqual(outer_values[outer_join_idx], inner_values[inner_join_idx])) {
                continue;
            }
            if (!MatchesAll(*inner_table, inner_values, jp.inner_residual_predicates)) continue;

            if (plan.select_star) {
                std::vector<LiteralValue> row = outer_values;
                row.insert(row.end(), inner_values.begin(), inner_values.end());
                result.rows.push_back(std::move(row));
                continue;
            }
            std::vector<LiteralValue> projected;
            projected.reserve(plan.select_list.size());
            for (const ColumnRef& col : plan.select_list) {
                bool from_outer = (*col.table_name == outer_table->name);
                const TableInfo& side_table = from_outer ? *outer_table : *inner_table;
                const std::vector<LiteralValue>& side_values = from_outer ? outer_values : inner_values;
                projected.push_back(side_values[ColumnIndex(side_table, col.column_name)]);
            }
            result.rows.push_back(std::move(projected));
        }
    }
    return result;
}

ExecuteResult ExecuteSelect(Database& db, const SelectStatement& stmt) {
    SelectPlan plan = PlanSelect(db.GetCatalog(), stmt);
    if (std::holds_alternative<TableAccessPlan>(plan.access)) {
        return ExecuteSelectSingleTable(db, plan, std::get<TableAccessPlan>(plan.access));
    }
    return ExecuteSelectJoin(db, plan, std::get<JoinPlan>(plan.access));
}

}  // namespace

ExecuteResult ExecuteInCurrentTransaction(Database& db, const Statement& stmt) {
    return std::visit(
        [&db](const auto& s) -> ExecuteResult {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, CreateTableStatement>) {
                return ExecuteCreateTable(db, s);
            } else if constexpr (std::is_same_v<T, CreateIndexStatement>) {
                return ExecuteCreateIndex(db, s);
            } else if constexpr (std::is_same_v<T, InsertStatement>) {
                return ExecuteInsert(db, s);
            } else if constexpr (std::is_same_v<T, SelectStatement>) {
                return ExecuteSelect(db, s);
            } else if constexpr (std::is_same_v<T, UpdateStatement>) {
                return ExecuteUpdate(db, s);
            } else if constexpr (std::is_same_v<T, DeleteStatement>) {
                return ExecuteDelete(db, s);
            } else {
                // BeginStatement / CommitStatement / RollbackStatement --
                // out of scope for this function entirely, see executor.h's
                // own class comment: session.h's Execute is what actually
                // handles these, and it never forwards one of these three
                // down to here.
                static_assert(std::is_same_v<T, BeginStatement> || std::is_same_v<T, CommitStatement> ||
                                  std::is_same_v<T, RollbackStatement>,
                              "ExecuteInCurrentTransaction: every Statement alternative must be handled above or "
                              "listed here");
                throw std::logic_error(
                    "ExecuteInCurrentTransaction: BEGIN/COMMIT/ROLLBACK have no data-manipulation meaning of "
                    "their own -- call session.h's Execute instead, which handles them directly");
            }
        },
        stmt);
}

}  // namespace flintdb
