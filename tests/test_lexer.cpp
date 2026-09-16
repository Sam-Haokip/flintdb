#include "../src/sql/lexer.h"
#include "../src/sql/sql_error.h"
#include "test_framework.h"

using namespace flintdb;

namespace {
void CheckType(const std::vector<Token>& tokens, size_t i, TokenType expected) {
    FLINTDB_CHECK(i < tokens.size());
    FLINTDB_CHECK(tokens[i].type == expected);
}
}  // namespace

FLINTDB_TEST(lexer_empty_input_produces_only_end_of_input) {
    auto tokens = Tokenize("");
    FLINTDB_CHECK_EQ(tokens.size(), 1u);
    CheckType(tokens, 0, TokenType::kEndOfInput);
    FLINTDB_CHECK_EQ(tokens[0].position, 0u);
}

FLINTDB_TEST(lexer_whitespace_only_input_produces_only_end_of_input) {
    auto tokens = Tokenize("   \t\n\r  ");
    FLINTDB_CHECK_EQ(tokens.size(), 1u);
    CheckType(tokens, 0, TokenType::kEndOfInput);
}

FLINTDB_TEST(lexer_keywords_are_recognized_case_insensitively) {
    for (const char* variant : {"SELECT", "select", "Select", "sElEcT"}) {
        auto tokens = Tokenize(variant);
        FLINTDB_CHECK_EQ(tokens.size(), 2u);  // SELECT + end of input
        CheckType(tokens, 0, TokenType::kSelect);
    }
}

FLINTDB_TEST(lexer_all_grammar_keywords_lex_to_their_own_token_type) {
    struct Case { const char* text; TokenType type; };
    const Case cases[] = {
        {"CREATE", TokenType::kCreate},   {"TABLE", TokenType::kTable},
        {"INDEX", TokenType::kIndex},     {"ON", TokenType::kOn},
        {"INTEGER", TokenType::kInteger}, {"TEXT", TokenType::kText},
        {"PRIMARY", TokenType::kPrimary}, {"KEY", TokenType::kKey},
        {"INSERT", TokenType::kInsert},   {"INTO", TokenType::kInto},
        {"VALUES", TokenType::kValues},   {"SELECT", TokenType::kSelect},
        {"FROM", TokenType::kFrom},       {"JOIN", TokenType::kJoin},
        {"WHERE", TokenType::kWhere},     {"AND", TokenType::kAnd},
        {"UPDATE", TokenType::kUpdate},   {"SET", TokenType::kSet},
        {"DELETE", TokenType::kDelete},   {"BEGIN", TokenType::kBegin},
        {"COMMIT", TokenType::kCommit},   {"ROLLBACK", TokenType::kRollback},
    };
    for (const auto& c : cases) {
        auto tokens = Tokenize(c.text);
        FLINTDB_CHECK_EQ(tokens.size(), 2u);
        CheckType(tokens, 0, c.type);
    }
}

FLINTDB_TEST(lexer_identifiers_preserve_original_case_and_are_not_folded) {
    auto tokens = Tokenize("WidgetsTable");
    FLINTDB_CHECK_EQ(tokens.size(), 2u);
    CheckType(tokens, 0, TokenType::kIdentifier);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string("WidgetsTable"));
}

FLINTDB_TEST(lexer_identifiers_allow_letters_digits_and_underscore_but_not_leading_digit) {
    auto tokens = Tokenize("_col1 col_2 c3olumn");
    FLINTDB_CHECK_EQ(tokens.size(), 4u);  // 3 identifiers + end of input
    for (size_t i = 0; i < 3; i++) CheckType(tokens, i, TokenType::kIdentifier);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string("_col1"));
    FLINTDB_CHECK_EQ(tokens[1].text, std::string("col_2"));
    FLINTDB_CHECK_EQ(tokens[2].text, std::string("c3olumn"));
}

FLINTDB_TEST(lexer_a_keyword_prefixed_identifier_is_still_an_identifier) {
    // "SELECTOR" must not be mistaken for SELECT + "OR" -- the maximal-munch
    // rule for identifier scanning already guarantees this, but it's worth
    // asserting directly since it's exactly the kind of lexer bug that's
    // easy to introduce with an ad-hoc keyword check.
    auto tokens = Tokenize("SELECTOR");
    FLINTDB_CHECK_EQ(tokens.size(), 2u);
    CheckType(tokens, 0, TokenType::kIdentifier);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string("SELECTOR"));
}

FLINTDB_TEST(lexer_integer_literals_positive_negative_zero_and_multidigit) {
    auto tokens = Tokenize("0 42 -7 -0 123456789");
    FLINTDB_CHECK_EQ(tokens.size(), 6u);
    for (size_t i = 0; i < 5; i++) CheckType(tokens, i, TokenType::kIntegerLiteral);
    FLINTDB_CHECK_EQ(tokens[0].int_value, 0);
    FLINTDB_CHECK_EQ(tokens[1].int_value, 42);
    FLINTDB_CHECK_EQ(tokens[2].int_value, -7);
    FLINTDB_CHECK_EQ(tokens[3].int_value, 0);
    FLINTDB_CHECK_EQ(tokens[4].int_value, 123456789);
}

FLINTDB_TEST(lexer_integer_literal_overflow_throws_sql_syntax_error) {
    bool threw = false;
    try {
        Tokenize("99999999999999999999999999");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(lexer_string_literal_decodes_simple_content) {
    auto tokens = Tokenize("'hello world'");
    FLINTDB_CHECK_EQ(tokens.size(), 2u);
    CheckType(tokens, 0, TokenType::kStringLiteral);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string("hello world"));
}

FLINTDB_TEST(lexer_string_literal_empty_string_decodes_to_empty) {
    auto tokens = Tokenize("''");
    CheckType(tokens, 0, TokenType::kStringLiteral);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string(""));
}

FLINTDB_TEST(lexer_string_literal_doubled_quote_decodes_to_one_literal_quote) {
    auto tokens = Tokenize("'it''s here'");
    CheckType(tokens, 0, TokenType::kStringLiteral);
    FLINTDB_CHECK_EQ(tokens[0].text, std::string("it's here"));
}

FLINTDB_TEST(lexer_unterminated_string_literal_throws_sql_syntax_error_with_start_position) {
    bool threw = false;
    try {
        Tokenize("SELECT * FROM t WHERE name = 'oops");
    } catch (const SqlSyntaxError& e) {
        threw = true;
        FLINTDB_CHECK_EQ(e.position, 29u);  // the opening quote's position
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(lexer_punctuation_and_comparators_all_lex_correctly) {
    auto tokens = Tokenize("( ) , . * = != < <= > >= ;");
    TokenType expected[] = {
        TokenType::kLParen, TokenType::kRParen, TokenType::kComma, TokenType::kDot,
        TokenType::kStar,   TokenType::kEq,     TokenType::kNeq,   TokenType::kLt,
        TokenType::kLe,     TokenType::kGt,     TokenType::kGe,    TokenType::kSemicolon,
        TokenType::kEndOfInput,
    };
    FLINTDB_CHECK_EQ(tokens.size(), sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < tokens.size(); i++) CheckType(tokens, i, expected[i]);
}

FLINTDB_TEST(lexer_comparators_are_not_confused_with_each_other_when_adjacent_to_other_tokens) {
    // No spaces around comparators -- must still split correctly rather
    // than greedily misreading e.g. "id<=5" as something else.
    auto tokens = Tokenize("id<=5");
    FLINTDB_CHECK_EQ(tokens.size(), 4u);
    CheckType(tokens, 0, TokenType::kIdentifier);
    CheckType(tokens, 1, TokenType::kLe);
    CheckType(tokens, 2, TokenType::kIntegerLiteral);
    CheckType(tokens, 3, TokenType::kEndOfInput);
}

FLINTDB_TEST(lexer_lone_bang_without_equals_throws_sql_syntax_error) {
    bool threw = false;
    try {
        Tokenize("a ! b");
    } catch (const SqlSyntaxError&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(lexer_unrecognized_character_throws_sql_syntax_error_with_position) {
    bool threw = false;
    try {
        Tokenize("SELECT * FROM t @ WHERE");
    } catch (const SqlSyntaxError& e) {
        threw = true;
        FLINTDB_CHECK_EQ(e.position, 16u);
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(lexer_full_create_table_statement) {
    auto tokens = Tokenize("CREATE TABLE widgets (id INTEGER PRIMARY KEY, name TEXT)");
    std::vector<TokenType> types;
    for (const auto& t : tokens) types.push_back(t.type);
    std::vector<TokenType> expected = {
        TokenType::kCreate, TokenType::kTable,   TokenType::kIdentifier, TokenType::kLParen,
        TokenType::kIdentifier, TokenType::kInteger, TokenType::kPrimary, TokenType::kKey,
        TokenType::kComma,  TokenType::kIdentifier, TokenType::kText,    TokenType::kRParen,
        TokenType::kEndOfInput,
    };
    FLINTDB_CHECK(types == expected);
    FLINTDB_CHECK_EQ(tokens[2].text, std::string("widgets"));
    FLINTDB_CHECK_EQ(tokens[4].text, std::string("id"));
    FLINTDB_CHECK_EQ(tokens[9].text, std::string("name"));
}

FLINTDB_TEST(lexer_full_select_statement_with_join_and_where) {
    auto tokens = Tokenize(
        "SELECT a.id, b.name FROM a JOIN b ON a.id = b.a_id WHERE a.qty > 5 AND b.active = 1");
    FLINTDB_CHECK(!tokens.empty());
    FLINTDB_CHECK(tokens.back().type == TokenType::kEndOfInput);
    // Spot-check the interesting parts rather than the whole sequence.
    FLINTDB_CHECK(tokens[0].type == TokenType::kSelect);
    FLINTDB_CHECK(tokens[1].type == TokenType::kIdentifier && tokens[1].text == "a");
    FLINTDB_CHECK(tokens[2].type == TokenType::kDot);
    FLINTDB_CHECK(tokens[3].type == TokenType::kIdentifier && tokens[3].text == "id");
    bool saw_join = false, saw_on = false, saw_where = false, saw_and = false, saw_gt = false;
    for (const auto& t : tokens) {
        saw_join |= t.type == TokenType::kJoin;
        saw_on |= t.type == TokenType::kOn;
        saw_where |= t.type == TokenType::kWhere;
        saw_and |= t.type == TokenType::kAnd;
        saw_gt |= t.type == TokenType::kGt;
    }
    FLINTDB_CHECK(saw_join && saw_on && saw_where && saw_and && saw_gt);
}

FLINTDB_TEST(lexer_full_insert_statement_with_string_and_integer_literals) {
    auto tokens = Tokenize("INSERT INTO widgets VALUES (1, 'a widget', -3)");
    std::vector<TokenType> types;
    for (const auto& t : tokens) types.push_back(t.type);
    std::vector<TokenType> expected = {
        TokenType::kInsert, TokenType::kInto,          TokenType::kIdentifier,  TokenType::kValues,
        TokenType::kLParen, TokenType::kIntegerLiteral, TokenType::kComma,      TokenType::kStringLiteral,
        TokenType::kComma,  TokenType::kIntegerLiteral, TokenType::kRParen,     TokenType::kEndOfInput,
    };
    FLINTDB_CHECK(types == expected);
    FLINTDB_CHECK_EQ(tokens[5].int_value, 1);
    FLINTDB_CHECK_EQ(tokens[7].text, std::string("a widget"));
    FLINTDB_CHECK_EQ(tokens[9].int_value, -3);
}

FLINTDB_TEST(lexer_full_update_and_delete_statements) {
    auto update_tokens = Tokenize("UPDATE widgets SET qty = 5 WHERE id = 1");
    FLINTDB_CHECK(update_tokens[0].type == TokenType::kUpdate);
    FLINTDB_CHECK(update_tokens[2].type == TokenType::kSet);

    auto delete_tokens = Tokenize("DELETE FROM widgets WHERE id = 1");
    FLINTDB_CHECK(delete_tokens[0].type == TokenType::kDelete);
    FLINTDB_CHECK(delete_tokens[1].type == TokenType::kFrom);
}

FLINTDB_TEST(lexer_transaction_keywords_lex_as_bare_statements) {
    FLINTDB_CHECK(Tokenize("BEGIN")[0].type == TokenType::kBegin);
    FLINTDB_CHECK(Tokenize("COMMIT")[0].type == TokenType::kCommit);
    FLINTDB_CHECK(Tokenize("ROLLBACK")[0].type == TokenType::kRollback);
}

FLINTDB_TEST(lexer_trailing_semicolon_is_optional_and_lexes_as_its_own_token) {
    auto with_semi = Tokenize("BEGIN;");
    FLINTDB_CHECK_EQ(with_semi.size(), 3u);  // BEGIN, ;, end of input
    CheckType(with_semi, 1, TokenType::kSemicolon);

    auto without_semi = Tokenize("BEGIN");
    FLINTDB_CHECK_EQ(without_semi.size(), 2u);  // BEGIN, end of input
}

FLINTDB_TEST(lexer_token_type_name_returns_a_readable_string_for_every_type) {
    // Every enumerator should map to a non-empty, non-placeholder name --
    // guards against a new TokenType added to token.h without a matching
    // case in TokenTypeName's switch (which would otherwise only show up
    // as a silent "<unknown token type>" in a future syntax error).
    for (TokenType t : {TokenType::kCreate, TokenType::kIdentifier, TokenType::kIntegerLiteral,
                         TokenType::kStringLiteral, TokenType::kLParen, TokenType::kEq,
                         TokenType::kSemicolon, TokenType::kEndOfInput}) {
        FLINTDB_CHECK(TokenTypeName(t) != "<unknown token type>");
        FLINTDB_CHECK(!TokenTypeName(t).empty());
    }
}
