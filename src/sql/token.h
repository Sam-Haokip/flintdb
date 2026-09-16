#pragma once
#include <cstdint>
#include <string>

namespace flintdb {

// One token kind per keyword in docs/SPEC.md section 1.2's grammar, plus
// BEGIN/COMMIT/ROLLBACK (section 1.3's transaction statements -- their
// own grammar isn't spelled out until task #44, but the lexer needs to
// recognize the three keywords now regardless of what statement shape
// the parser eventually builds around them), the three literal/name
// kinds, the grammar's punctuation and comparators, an optional trailing
// statement terminator, and end-of-input. See docs/DECISIONS.md D-044
// for the lexical conventions (case-folding, escaping, etc.) behind this
// set.
enum class TokenType {
    // Keywords (matched case-insensitively -- D-044).
    kCreate,
    kTable,
    kIndex,
    kOn,
    kInteger,
    kText,
    kPrimary,
    kKey,
    kInsert,
    kInto,
    kValues,
    kSelect,
    kFrom,
    kJoin,
    kWhere,
    kAnd,
    kUpdate,
    kSet,
    kDelete,
    kBegin,
    kCommit,
    kRollback,

    // Names and literals.
    kIdentifier,       // text = exact source spelling, case preserved
    kIntegerLiteral,   // int_value = the parsed value (sign included, D-044)
    kStringLiteral,    // text = the decoded value ('' already unescaped to ')

    // Punctuation.
    kLParen,
    kRParen,
    kComma,
    kDot,
    kStar,

    // Comparators.
    kEq,
    kNeq,
    kLt,
    kLe,
    kGt,
    kGe,

    kSemicolon,
    kEndOfInput,
};

// One lexed token. Only the fields relevant to `type` are meaningful:
// `text` for kIdentifier/kStringLiteral, `int_value` for
// kIntegerLiteral; every other token type carries no payload beyond its
// type and position. `position` is the byte offset of the token's first
// character in the original source string, used only for error messages
// (SqlSyntaxError, sql_error.h) -- never compared or relied on for
// correctness.
struct Token {
    TokenType type;
    std::string text;
    int64_t int_value = 0;
    size_t position = 0;
};

// A short, human-readable name for `type` -- used in SqlSyntaxError
// messages (lexer.cpp, and the parser, task #41) so a syntax error can
// say "expected FROM, got IDENTIFIER" rather than an opaque enum value.
std::string TokenTypeName(TokenType type);

}  // namespace flintdb
