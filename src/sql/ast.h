#pragma once
#include "../catalog/catalog.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace flintdb {

// One literal value -- an INTEGER or a TEXT constant (docs/SPEC.md
// section 1.1: exactly these two types, nothing else). A plain
// enum-plus-fields struct rather than std::variant<int64_t, std::string>,
// matching this project's general preference for explicit fields (see
// Database::ObjectStorage, docs/DECISIONS.md D-034's header) -- unlike
// Statement below, there's no meaningful benefit to variant's
// exhaustiveness-checking for a plain two-case value type callers mostly
// just want to read out of.
struct LiteralValue {
    enum class Kind { kInteger, kString };
    Kind kind;
    int64_t int_value = 0;
    std::string string_value;
};

// [table_name "."] column_name (docs/SPEC.md section 1.2's column_ref).
// table_name is set only when the SQL text used the qualified
// "table.column" form -- relevant once a query has a JOIN and a WHERE/
// select-list column could belong to either side.
struct ColumnRef {
    std::optional<std::string> table_name;
    std::string column_name;
};

enum class Comparator { kEq, kNeq, kLt, kLe, kGt, kGe };

// column_ref comparator literal (docs/SPEC.md section 1.2's predicate).
struct Predicate {
    ColumnRef column;
    Comparator op;
    LiteralValue value;
};

// column_name "=" literal (docs/SPEC.md section 1.2's assignment, used
// only by UPDATE's SET clause). Plain column_name, not a ColumnRef --
// UPDATE always targets exactly one table, so there's no "table.column"
// ambiguity to resolve here the way there is in a JOIN's WHERE clause.
struct Assignment {
    std::string column_name;
    LiteralValue value;
};

// "JOIN" table_name "ON" column_ref "=" column_ref. No stored comparator
// -- see docs/DECISIONS.md D-045 for why: docs/SPEC.md section 1.3
// restricts JOIN to equi-join only, so the parser requires and consumes a
// literal '=' rather than recording an operator that could only ever be
// one value.
struct JoinClause {
    std::string table_name;
    ColumnRef left_column;
    ColumnRef right_column;
};

// CREATE TABLE table_name (column_def, ...). `columns` and
// `primary_key_column` are deliberately the exact parameter shape
// Catalog::CreateTable already takes (catalog.h) -- see D-045 -- so the
// executor can forward them directly with no translation step. The
// parser enforces "at least one column" (the grammar's own shape) and
// "at most one PRIMARY KEY" (the AST has only one slot for it); it does
// NOT check for duplicate column names or a PRIMARY KEY naming a
// nonexistent column -- Catalog::CreateTable already validates both
// (D-045).
struct CreateTableStatement {
    std::string table_name;
    std::vector<ColumnDef> columns;
    std::optional<std::string> primary_key_column;
};

// CREATE INDEX index_name ON table_name (column_name) -- exactly
// Catalog::CreateIndex's parameter shape (D-045).
struct CreateIndexStatement {
    std::string index_name;
    std::string table_name;
    std::string column_name;
};

// INSERT INTO table_name VALUES (literal, ...). Column order/count
// matching the table's schema is a semantic check (the executor's job,
// task #43, once it has the Catalog's TableInfo to check against) -- the
// parser only knows this is a nonempty list of literals.
struct InsertStatement {
    std::string table_name;
    std::vector<LiteralValue> values;
};

// SELECT select_list FROM table_name [JOIN ...] [WHERE ...].
// select_star == true means "SELECT *" (select_list is then empty and
// unused); otherwise select_list holds the explicit column_ref list,
// which is never empty (the grammar's column_ref ("," column_ref)* makes
// the first one mandatory whenever '*' wasn't used).
struct SelectStatement {
    bool select_star = false;
    std::vector<ColumnRef> select_list;
    std::string from_table;
    std::optional<JoinClause> join;
    std::vector<Predicate> where_predicates;  // AND-conjunction; empty means no WHERE at all
};

// UPDATE table_name SET assignment, ... [WHERE ...].
struct UpdateStatement {
    std::string table_name;
    std::vector<Assignment> assignments;
    std::vector<Predicate> where_predicates;
};

// DELETE FROM table_name [WHERE ...].
struct DeleteStatement {
    std::string table_name;
    std::vector<Predicate> where_predicates;
};

// BEGIN / COMMIT / ROLLBACK (docs/SPEC.md section 1.3) -- bare
// statements, no fields of their own. Kept as distinct empty struct
// types (rather than, say, an enum) so they participate in the
// Statement variant the same uniform way every other statement does.
struct BeginStatement {};
struct CommitStatement {};
struct RollbackStatement {};

// Exactly one parsed statement -- see docs/DECISIONS.md D-045 for why
// this is a std::variant despite this project's general preference for
// plain fields elsewhere: unlike a two-case value type, a 9-way tagged
// sum type with no shared fields between alternatives is precisely what
// variant (with std::visit/std::get_if) is for.
using Statement =
    std::variant<CreateTableStatement, CreateIndexStatement, InsertStatement, SelectStatement,
                 UpdateStatement, DeleteStatement, BeginStatement, CommitStatement, RollbackStatement>;

}  // namespace flintdb
