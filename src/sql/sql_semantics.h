#pragma once
#include "../catalog/catalog.h"
#include "ast.h"

#include <cstddef>
#include <string>

namespace flintdb {

// Small semantic-resolution helpers shared by the planner (planner.h/.cpp,
// resolving predicate/select_list/join columns) and the executor
// (executor.h/.cpp, resolving INSERT/UPDATE target columns) -- pulled out
// once the executor needed the exact same "does this column exist, does
// this literal match its type" checks the planner already had, rather
// than let a second copy drift out of sync with the first (the same
// "don't duplicate validation logic" reasoning docs/DECISIONS.md D-045
// already applied to the parser vs. Catalog).

// Throws SqlSemanticError (sql_error.h) if `table` has no column named
// `column_name`; otherwise returns it.
const ColumnDef& RequireColumn(const TableInfo& table, const std::string& column_name);

// Throws SqlSemanticError if `value`'s kind doesn't match `col`'s declared
// type -- docs/SPEC.md section 1.1 has exactly two types and no coercion
// between them, so this is a strict match, not a "can this be converted"
// check.
void RequireLiteralMatchesColumn(const TableInfo& table, const ColumnDef& col, const LiteralValue& value);

// `column_name`'s zero-based position within `table.columns` -- the
// position EncodeRow/DecodeRow (row_codec.h) store/read it at, and the
// index a caller uses to pull that column's value out of a DecodeRow
// result. Throws SqlSemanticError if `table` has no such column (calls
// RequireColumn internally, so callers that already have the ColumnDef
// from a prior RequireColumn call still get the same check, just
// harmlessly repeated).
size_t ColumnIndex(const TableInfo& table, const std::string& column_name);

}  // namespace flintdb
