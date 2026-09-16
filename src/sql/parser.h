#pragma once
#include "ast.h"

#include <string_view>

namespace flintdb {

// Tokenizes and parses exactly one SQL statement (docs/SPEC.md section
// 1.2/1.3's grammar) from `sql`, returning its AST (ast.h). An optional
// trailing ';' is consumed if present (docs/DECISIONS.md D-044); anything
// else left over after the statement -- including a second statement --
// is a syntax error, since this engine parses and executes one statement
// per call (there is no multi-statement script mode).
//
// Throws SqlSyntaxError (sql_error.h) for anything that doesn't match the
// grammar's *shape* -- a missing keyword, an unexpected token, more than
// one PRIMARY KEY column, trailing garbage after the statement. Does NOT
// check SQL *semantics* (does this table exist, are these column names
// unique, does this INSERT have the right number of values for the
// table) -- see docs/DECISIONS.md D-045 for why that's deliberately left
// to Catalog and the executor (task #43), which are the layers that
// actually have the schema information needed to check it.
Statement Parse(std::string_view sql);

}  // namespace flintdb
