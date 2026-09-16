#pragma once
#include "ast.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace flintdb {

class Catalog;

// How a single table's rows are found: either every page in its HeapFile
// (kSeqScan) or a point lookup into one of its indexes (kIndexScan). See
// docs/DECISIONS.md D-047 for the fixed rule that decides between them --
// there is no cost model anywhere in this engine, so this is genuinely a
// binary choice, not a ranking among several candidates.
enum class ScanMethod { kSeqScan, kIndexScan };

// One table's access path, fully self-contained: everything the executor
// needs to find this table's matching rows, with no per-row information
// supplied from outside (contrast JoinPlan's `inner_*` fields, D-047,
// which are NOT self-contained in this sense -- their key comes from the
// outer loop, not from here).
//
// `residual_predicates` is every predicate from the original WHERE clause
// that still needs to be checked against a candidate row after it's
// fetched: every predicate, verbatim, when `method == kSeqScan`; every
// predicate except whichever one drove the index probe when
// `method == kIndexScan` (re-checking that one is pure waste -- an
// index probe's key match is already exact, see D-047). docs/SPEC.md
// section 1.3's WHERE is AND-only, so a plain post-fetch filter over
// whatever's left is always correct.
struct TableAccessPlan {
    std::string table_name;
    ScanMethod method = ScanMethod::kSeqScan;

    // Set iff method == kIndexScan.
    std::string index_name;
    int64_t index_key = 0;

    std::vector<Predicate> residual_predicates;
};

// SELECT ... JOIN ...: outer is always the FROM table, inner is always
// the JOIN table (docs/SPEC.md section 4: "no join reordering" -- this
// planner never considers driving from the other side). The executor
// scans `outer` exactly as TableAccessPlan describes, and for every
// surviving outer row, probes or scans `inner`:
//
//   - inner_method == kIndexScan: look up inner_index_name with the key
//     = the outer row's value at outer_join_column (a fresh key on every
//     iteration -- this is why inner has no static index_key of its own,
//     unlike TableAccessPlan).
//   - inner_method == kSeqScan: scan every row of the inner table,
//     keeping only those whose inner_join_column value equals the outer
//     row's outer_join_column value (the executor must apply this
//     equality check explicitly in this branch; the kIndexScan branch
//     gets it for free from the index probe itself).
//
// Either way, `inner_residual_predicates` (the inner table's own WHERE
// predicates, if any) are then applied as a plain post-fetch filter --
// D-047: these never influence inner_method, which is always chosen by
// whether an index exists on the join column, not on any of these.
struct JoinPlan {
    TableAccessPlan outer;

    std::string inner_table_name;
    ScanMethod inner_method = ScanMethod::kSeqScan;
    std::string inner_index_name;  // set iff inner_method == kIndexScan
    std::vector<Predicate> inner_residual_predicates;

    std::string outer_join_column;  // a plain column name -- outer.table_name already says which table
    std::string inner_join_column;  // a plain column name -- inner_table_name already says which table
};

// A fully planned SELECT. `select_list`'s ColumnRefs always have
// table_name populated here (unlike the parsed SelectStatement's, where
// it's optional) -- see docs/DECISIONS.md D-047 for why: once a JOIN is
// present, the executor's output row is really a pair of two separate
// table rows, and needs to know unambiguously which one each output
// column comes from. Unused (left empty) when select_star is true,
// mirroring SelectStatement's own convention (ast.h) -- the executor
// expands `SELECT *` itself directly off Catalog::TableInfo::columns
// (outer's columns then inner's, for a join), with no planner
// involvement needed.
struct SelectPlan {
    bool select_star = false;
    std::vector<ColumnRef> select_list;
    std::variant<TableAccessPlan, JoinPlan> access;
};

// Plans how to find the rows of `table_name` matching every predicate in
// `predicates` (an AND-conjunction, docs/SPEC.md section 1.3) -- the one
// primitive both PlanSelect (for a join-free SELECT, or a JOIN's outer
// side) and the executor (task #43, for UPDATE/DELETE's WHERE clause)
// build on. See docs/DECISIONS.md D-047 for the fixed index-vs-scan rule
// and its deterministic tie-break.
//
// Throws SqlSemanticError (sql_error.h) if `table_name` doesn't name an
// existing table, any predicate names a column that table doesn't have,
// or a predicate's literal kind doesn't match its column's declared type.
TableAccessPlan PlanTableAccess(const Catalog& catalog, const std::string& table_name,
                                 const std::vector<Predicate>& predicates);

// Plans a full SELECT statement: the FROM table's (and, if present, the
// JOIN table's) access path, plus select_list resolution. See
// docs/DECISIONS.md D-047 for the equi-join planning rule (outer=FROM,
// inner=JOIN, inner prefers an index on the join column) and the two
// structural rejections (JOIN comparing a table to itself, or two columns
// of different types).
//
// Throws SqlSemanticError for everything PlanTableAccess does, plus: an
// unknown JOIN table, a self-join, a JOIN ON clause whose two columns
// don't resolve to different tables or don't share a ColumnType, or a
// select_list column that's unknown or ambiguous between the two tables.
SelectPlan PlanSelect(const Catalog& catalog, const SelectStatement& stmt);

}  // namespace flintdb
