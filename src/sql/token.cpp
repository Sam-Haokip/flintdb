#include "token.h"

namespace flintdb {

std::string TokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::kCreate: return "CREATE";
        case TokenType::kTable: return "TABLE";
        case TokenType::kIndex: return "INDEX";
        case TokenType::kOn: return "ON";
        case TokenType::kInteger: return "INTEGER";
        case TokenType::kText: return "TEXT";
        case TokenType::kPrimary: return "PRIMARY";
        case TokenType::kKey: return "KEY";
        case TokenType::kInsert: return "INSERT";
        case TokenType::kInto: return "INTO";
        case TokenType::kValues: return "VALUES";
        case TokenType::kSelect: return "SELECT";
        case TokenType::kFrom: return "FROM";
        case TokenType::kJoin: return "JOIN";
        case TokenType::kWhere: return "WHERE";
        case TokenType::kAnd: return "AND";
        case TokenType::kUpdate: return "UPDATE";
        case TokenType::kSet: return "SET";
        case TokenType::kDelete: return "DELETE";
        case TokenType::kBegin: return "BEGIN";
        case TokenType::kCommit: return "COMMIT";
        case TokenType::kRollback: return "ROLLBACK";
        case TokenType::kIdentifier: return "IDENTIFIER";
        case TokenType::kIntegerLiteral: return "INTEGER_LITERAL";
        case TokenType::kStringLiteral: return "STRING_LITERAL";
        case TokenType::kLParen: return "'('";
        case TokenType::kRParen: return "')'";
        case TokenType::kComma: return "','";
        case TokenType::kDot: return "'.'";
        case TokenType::kStar: return "'*'";
        case TokenType::kEq: return "'='";
        case TokenType::kNeq: return "'!='";
        case TokenType::kLt: return "'<'";
        case TokenType::kLe: return "'<='";
        case TokenType::kGt: return "'>'";
        case TokenType::kGe: return "'>='";
        case TokenType::kSemicolon: return "';'";
        case TokenType::kEndOfInput: return "end of input";
    }
    return "<unknown token type>";
}

}  // namespace flintdb
