#include "parser.h"

#include "lexer.h"
#include "sql_error.h"

namespace flintdb {

namespace {

// Recursive-descent parser over a fixed token vector (lexer.h always
// terminates it with exactly one kEndOfInput, so Peek() is always valid
// without a separate "am I at the end" check anywhere below).
class Parser {
 public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement ParseStatementTopLevel() {
        Statement stmt = ParseOneStatement();
        Match(TokenType::kSemicolon);  // optional trailing ';', D-044
        if (!Check(TokenType::kEndOfInput)) {
            throw SqlSyntaxError(
                "unexpected trailing input after statement (got " + TokenTypeName(Peek().type) + ")",
                Peek().position);
        }
        return stmt;
    }

 private:
    const Token& Peek() const { return tokens_[pos_]; }

    bool Check(TokenType type) const { return Peek().type == type; }

    const Token& Advance() {
        const Token& tok = tokens_[pos_];
        // Never step past the terminal kEndOfInput -- every real token
        // sequence ends with exactly one, so this is the only guard
        // needed to make repeated Advance() calls always safe.
        if (tok.type != TokenType::kEndOfInput) pos_++;
        return tok;
    }

    bool Match(TokenType type) {
        if (!Check(type)) return false;
        Advance();
        return true;
    }

    const Token& Expect(TokenType type) {
        if (!Check(type)) {
            throw SqlSyntaxError(
                "expected " + TokenTypeName(type) + ", got " + TokenTypeName(Peek().type), Peek().position);
        }
        return Advance();
    }

    std::string ExpectIdentifier() { return Expect(TokenType::kIdentifier).text; }

    LiteralValue ExpectLiteral() {
        if (Check(TokenType::kIntegerLiteral)) {
            LiteralValue v;
            v.kind = LiteralValue::Kind::kInteger;
            v.int_value = Advance().int_value;
            return v;
        }
        if (Check(TokenType::kStringLiteral)) {
            LiteralValue v;
            v.kind = LiteralValue::Kind::kString;
            v.string_value = Advance().text;
            return v;
        }
        throw SqlSyntaxError("expected a literal value, got " + TokenTypeName(Peek().type), Peek().position);
    }

    ColumnType ExpectColumnType() {
        if (Match(TokenType::kInteger)) return ColumnType::kInteger;
        if (Match(TokenType::kText)) return ColumnType::kText;
        throw SqlSyntaxError("expected INTEGER or TEXT, got " + TokenTypeName(Peek().type), Peek().position);
    }

    // [table_name "."] column_name.
    ColumnRef ParseColumnRef() {
        std::string first = ExpectIdentifier();
        if (Match(TokenType::kDot)) {
            ColumnRef ref;
            ref.table_name = first;
            ref.column_name = ExpectIdentifier();
            return ref;
        }
        return ColumnRef{std::nullopt, first};
    }

    Comparator ExpectComparator() {
        if (Match(TokenType::kEq)) return Comparator::kEq;
        if (Match(TokenType::kNeq)) return Comparator::kNeq;
        if (Match(TokenType::kLt)) return Comparator::kLt;
        if (Match(TokenType::kLe)) return Comparator::kLe;
        if (Match(TokenType::kGt)) return Comparator::kGt;
        if (Match(TokenType::kGe)) return Comparator::kGe;
        throw SqlSyntaxError("expected a comparator (=, !=, <, <=, >, >=), got " + TokenTypeName(Peek().type),
                             Peek().position);
    }

    // column_ref comparator literal.
    Predicate ParsePredicate() {
        Predicate p;
        p.column = ParseColumnRef();
        p.op = ExpectComparator();
        p.value = ExpectLiteral();
        return p;
    }

    // "WHERE" predicate ("AND" predicate)* -- called only when the
    // caller already peeked kWhere; returns a nonempty vector.
    std::vector<Predicate> ParseWhereClause() {
        Expect(TokenType::kWhere);
        std::vector<Predicate> predicates;
        predicates.push_back(ParsePredicate());
        while (Match(TokenType::kAnd)) predicates.push_back(ParsePredicate());
        return predicates;
    }

    // [where_clause] -- empty vector if the next token isn't WHERE.
    std::vector<Predicate> ParseOptionalWhereClause() {
        if (Check(TokenType::kWhere)) return ParseWhereClause();
        return {};
    }

    CreateTableStatement ParseCreateTable() {
        Expect(TokenType::kTable);
        CreateTableStatement stmt;
        stmt.table_name = ExpectIdentifier();
        Expect(TokenType::kLParen);
        while (true) {
            std::string col_name = ExpectIdentifier();
            ColumnType col_type = ExpectColumnType();
            stmt.columns.push_back(ColumnDef{col_name, col_type});
            if (Match(TokenType::kPrimary)) {
                Expect(TokenType::kKey);
                if (stmt.primary_key_column.has_value()) {
                    throw SqlSyntaxError(
                        "a table may have at most one PRIMARY KEY (already set on '" +
                            *stmt.primary_key_column + "')",
                        Peek().position);
                }
                stmt.primary_key_column = col_name;
            }
            if (Match(TokenType::kComma)) continue;
            break;
        }
        Expect(TokenType::kRParen);
        return stmt;
    }

    CreateIndexStatement ParseCreateIndex() {
        Expect(TokenType::kIndex);
        CreateIndexStatement stmt;
        stmt.index_name = ExpectIdentifier();
        Expect(TokenType::kOn);
        stmt.table_name = ExpectIdentifier();
        Expect(TokenType::kLParen);
        stmt.column_name = ExpectIdentifier();
        Expect(TokenType::kRParen);
        return stmt;
    }

    InsertStatement ParseInsert() {
        Expect(TokenType::kInto);
        InsertStatement stmt;
        stmt.table_name = ExpectIdentifier();
        Expect(TokenType::kValues);
        Expect(TokenType::kLParen);
        stmt.values.push_back(ExpectLiteral());
        while (Match(TokenType::kComma)) stmt.values.push_back(ExpectLiteral());
        Expect(TokenType::kRParen);
        return stmt;
    }

    // "JOIN" table_name "ON" column_ref "=" column_ref -- called only
    // when the caller already peeked kJoin.
    JoinClause ParseJoinClause() {
        Expect(TokenType::kJoin);
        JoinClause join;
        join.table_name = ExpectIdentifier();
        Expect(TokenType::kOn);
        join.left_column = ParseColumnRef();
        // docs/SPEC.md section 1.3: JOIN is equi-join only -- require '='
        // literally rather than accepting any comparator (D-045).
        Expect(TokenType::kEq);
        join.right_column = ParseColumnRef();
        return join;
    }

    SelectStatement ParseSelect() {
        SelectStatement stmt;
        if (Match(TokenType::kStar)) {
            stmt.select_star = true;
        } else {
            stmt.select_list.push_back(ParseColumnRef());
            while (Match(TokenType::kComma)) stmt.select_list.push_back(ParseColumnRef());
        }
        Expect(TokenType::kFrom);
        stmt.from_table = ExpectIdentifier();
        if (Check(TokenType::kJoin)) stmt.join = ParseJoinClause();
        stmt.where_predicates = ParseOptionalWhereClause();
        return stmt;
    }

    UpdateStatement ParseUpdate() {
        UpdateStatement stmt;
        stmt.table_name = ExpectIdentifier();
        Expect(TokenType::kSet);
        auto parse_assignment = [this]() {
            Assignment a;
            a.column_name = ExpectIdentifier();
            Expect(TokenType::kEq);
            a.value = ExpectLiteral();
            return a;
        };
        stmt.assignments.push_back(parse_assignment());
        while (Match(TokenType::kComma)) stmt.assignments.push_back(parse_assignment());
        stmt.where_predicates = ParseOptionalWhereClause();
        return stmt;
    }

    DeleteStatement ParseDelete() {
        Expect(TokenType::kFrom);
        DeleteStatement stmt;
        stmt.table_name = ExpectIdentifier();
        stmt.where_predicates = ParseOptionalWhereClause();
        return stmt;
    }

    Statement ParseOneStatement() {
        if (Match(TokenType::kCreate)) {
            if (Check(TokenType::kTable)) return ParseCreateTable();
            if (Check(TokenType::kIndex)) return ParseCreateIndex();
            throw SqlSyntaxError("expected TABLE or INDEX after CREATE, got " + TokenTypeName(Peek().type),
                                 Peek().position);
        }
        if (Match(TokenType::kInsert)) return ParseInsert();
        if (Match(TokenType::kSelect)) return ParseSelect();
        if (Match(TokenType::kUpdate)) return ParseUpdate();
        if (Match(TokenType::kDelete)) return ParseDelete();
        if (Match(TokenType::kBegin)) return BeginStatement{};
        if (Match(TokenType::kCommit)) return CommitStatement{};
        if (Match(TokenType::kRollback)) return RollbackStatement{};

        throw SqlSyntaxError(
            "expected a statement (CREATE, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT, or "
            "ROLLBACK), got " +
                TokenTypeName(Peek().type),
            Peek().position);
    }

    std::vector<Token> tokens_;
    size_t pos_ = 0;
};

}  // namespace

Statement Parse(std::string_view sql) {
    Parser parser(Tokenize(sql));
    return parser.ParseStatementTopLevel();
}

}  // namespace flintdb
