#pragma once
#include "token.h"

#include <string_view>
#include <vector>

namespace flintdb {

// Splits `sql` into a sequence of Tokens, ending with exactly one
// kEndOfInput token (so a parser can always safely peek one token past
// the last real one without a separate "am I at the end" check). See
// docs/DECISIONS.md D-044 for the lexical conventions this implements:
// case-insensitive keywords, case-preserving identifiers, ''-escaped
// string literals, sign-inclusive integer literals, no comment syntax,
// and an optional trailing ';'.
//
// Throws SqlSyntaxError (sql_error.h) on an unrecognized character or an
// unterminated string literal -- both are routine "the user's SQL was
// invalid" outcomes, not internal engine bugs (see SqlSyntaxError's own
// comment for why that's a distinct exception type from everything else
// in this codebase).
std::vector<Token> Tokenize(std::string_view sql);

}  // namespace flintdb
