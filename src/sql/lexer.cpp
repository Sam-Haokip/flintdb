#include "lexer.h"

#include "sql_error.h"

#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace flintdb {

namespace {

// Every keyword in docs/SPEC.md's grammar (plus BEGIN/COMMIT/ROLLBACK,
// D-044), keyed by its all-uppercase spelling -- lookup upper-cases the
// candidate word first, which is what gives keywords their
// case-insensitive matching (D-044) without needing a case-insensitive
// hash/equality on the whole map.
const std::unordered_map<std::string, TokenType>& KeywordTable() {
    static const std::unordered_map<std::string, TokenType> table = {
        {"CREATE", TokenType::kCreate},     {"TABLE", TokenType::kTable},
        {"INDEX", TokenType::kIndex},       {"ON", TokenType::kOn},
        {"INTEGER", TokenType::kInteger},   {"TEXT", TokenType::kText},
        {"PRIMARY", TokenType::kPrimary},   {"KEY", TokenType::kKey},
        {"INSERT", TokenType::kInsert},     {"INTO", TokenType::kInto},
        {"VALUES", TokenType::kValues},     {"SELECT", TokenType::kSelect},
        {"FROM", TokenType::kFrom},         {"JOIN", TokenType::kJoin},
        {"WHERE", TokenType::kWhere},       {"AND", TokenType::kAnd},
        {"UPDATE", TokenType::kUpdate},     {"SET", TokenType::kSet},
        {"DELETE", TokenType::kDelete},     {"BEGIN", TokenType::kBegin},
        {"COMMIT", TokenType::kCommit},     {"ROLLBACK", TokenType::kRollback},
    };
    return table;
}

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentCont(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

std::string ToUpper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::vector<Token> Tokenize(std::string_view sql) {
    std::vector<Token> tokens;
    size_t pos = 0;
    const size_t n = sql.size();

    while (pos < n) {
        char c = sql[pos];

        // Whitespace: skip, no token produced.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pos++;
            continue;
        }

        size_t start = pos;

        // Identifier or keyword.
        if (IsIdentStart(c)) {
            size_t begin = pos;
            pos++;
            while (pos < n && IsIdentCont(sql[pos])) pos++;
            std::string word(sql.substr(begin, pos - begin));
            auto it = KeywordTable().find(ToUpper(word));
            if (it != KeywordTable().end()) {
                tokens.push_back(Token{it->second, word, 0, start});
            } else {
                tokens.push_back(Token{TokenType::kIdentifier, word, 0, start});
            }
            continue;
        }

        // Integer literal, sign included (D-044): a '-' only starts a
        // number if immediately followed by a digit, since there is no
        // other syntax in this grammar a bare '-' could belong to.
        if (IsDigit(c) || (c == '-' && pos + 1 < n && IsDigit(sql[pos + 1]))) {
            size_t begin = pos;
            if (c == '-') pos++;
            while (pos < n && IsDigit(sql[pos])) pos++;
            std::string digits(sql.substr(begin, pos - begin));
            try {
                int64_t value = std::stoll(digits);
                tokens.push_back(Token{TokenType::kIntegerLiteral, digits, value, start});
            } catch (const std::out_of_range&) {
                throw SqlSyntaxError("integer literal '" + digits + "' is out of range", start);
            }
            continue;
        }

        // String literal: single-quoted, '' is an escaped literal quote
        // (D-044) -- decoded into `text` so callers get the real value,
        // not source syntax.
        if (c == '\'') {
            size_t begin = pos;
            pos++;  // consume opening quote
            std::string value;
            bool closed = false;
            while (pos < n) {
                if (sql[pos] == '\'') {
                    if (pos + 1 < n && sql[pos + 1] == '\'') {
                        value.push_back('\'');
                        pos += 2;
                        continue;
                    }
                    pos++;  // consume closing quote
                    closed = true;
                    break;
                }
                value.push_back(sql[pos]);
                pos++;
            }
            if (!closed) {
                throw SqlSyntaxError("unterminated string literal starting here", begin);
            }
            tokens.push_back(Token{TokenType::kStringLiteral, value, 0, begin});
            continue;
        }

        // Single-character punctuation.
        switch (c) {
            case '(':
                tokens.push_back(Token{TokenType::kLParen, "(", 0, start});
                pos++;
                continue;
            case ')':
                tokens.push_back(Token{TokenType::kRParen, ")", 0, start});
                pos++;
                continue;
            case ',':
                tokens.push_back(Token{TokenType::kComma, ",", 0, start});
                pos++;
                continue;
            case '.':
                tokens.push_back(Token{TokenType::kDot, ".", 0, start});
                pos++;
                continue;
            case '*':
                tokens.push_back(Token{TokenType::kStar, "*", 0, start});
                pos++;
                continue;
            case ';':
                tokens.push_back(Token{TokenType::kSemicolon, ";", 0, start});
                pos++;
                continue;
            default:
                break;
        }

        // Comparators, some of which are two characters.
        if (c == '=') {
            tokens.push_back(Token{TokenType::kEq, "=", 0, start});
            pos++;
            continue;
        }
        if (c == '!') {
            if (pos + 1 < n && sql[pos + 1] == '=') {
                tokens.push_back(Token{TokenType::kNeq, "!=", 0, start});
                pos += 2;
                continue;
            }
            throw SqlSyntaxError("unexpected character '!' (did you mean '!='?)", start);
        }
        if (c == '<') {
            if (pos + 1 < n && sql[pos + 1] == '=') {
                tokens.push_back(Token{TokenType::kLe, "<=", 0, start});
                pos += 2;
            } else {
                tokens.push_back(Token{TokenType::kLt, "<", 0, start});
                pos++;
            }
            continue;
        }
        if (c == '>') {
            if (pos + 1 < n && sql[pos + 1] == '=') {
                tokens.push_back(Token{TokenType::kGe, ">=", 0, start});
                pos += 2;
            } else {
                tokens.push_back(Token{TokenType::kGt, ">", 0, start});
                pos++;
            }
            continue;
        }

        throw SqlSyntaxError(std::string("unrecognized character '") + c + "'", start);
    }

    tokens.push_back(Token{TokenType::kEndOfInput, "", 0, n});
    return tokens;
}

}  // namespace flintdb
