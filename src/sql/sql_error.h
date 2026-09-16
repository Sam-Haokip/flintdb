#pragma once
#include <stdexcept>
#include <string>

namespace flintdb {

// Thrown for malformed SQL text -- an unrecognized character or
// unterminated string literal (Tokenize, lexer.h) or a token sequence
// that doesn't match the grammar (the parser, task #41). Deliberately a
// distinct type from every other exception this codebase throws
// (std::invalid_argument, std::runtime_error, std::out_of_range,
// TransactionAbortedException): those all mean either a real caller/
// programming bug or a durability-critical fail-loud condition, never
// "the user's SQL text itself was invalid" -- which is an expected,
// routine, recoverable outcome the eventual executor/host needs to catch
// and report as a query error, not let propagate as an internal fault.
// See docs/DECISIONS.md D-044 for the full reasoning.
class SqlSyntaxError : public std::runtime_error {
 public:
    SqlSyntaxError(const std::string& message, size_t position)
        : std::runtime_error(message + " (at position " + std::to_string(position) + ")"), position(position) {}

    size_t position;
};

// Thrown for SQL that parses fine (a valid token sequence matching the
// grammar's shape, `SqlSyntaxError`'s domain) but doesn't *mean* anything
// this database can execute once checked against the `Catalog`'s actual
// schema: an unknown table or column, a column reference that's ambiguous
// between a `JOIN`'s two tables, a predicate literal whose kind doesn't
// match its column's declared type, or a `JOIN`'s `ON` clause that isn't a
// legal equi-join condition (docs/DECISIONS.md D-047 -- the planner,
// task #42, is what throws this). Deliberately a distinct type from
// `SqlSyntaxError` above, for the same reason D-045 keeps the parser's
// grammar-shape validation separate from `Catalog::CreateTable`/
// `CreateIndex`'s own semantic validation: "the token stream didn't match
// the grammar" and "the token stream parsed fine but doesn't resolve
// against this schema" are different failures a caller may want to
// distinguish, and conflating them would make a future `catch` ambiguous
// about which one it actually caught. Unlike `SqlSyntaxError`, there is no
// meaningful lexer `position` to attach here -- by the time semantic
// resolution happens, the AST no longer carries token positions -- so this
// carries only a message (via `std::runtime_error::what()`).
class SqlSemanticError : public std::runtime_error {
 public:
    explicit SqlSemanticError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace flintdb
