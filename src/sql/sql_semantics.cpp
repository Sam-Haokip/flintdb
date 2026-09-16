#include "sql_semantics.h"

#include "sql_error.h"

namespace flintdb {

namespace {

std::string LiteralKindName(LiteralValue::Kind kind) {
    return kind == LiteralValue::Kind::kInteger ? "INTEGER" : "TEXT";
}

}  // namespace

const ColumnDef& RequireColumn(const TableInfo& table, const std::string& column_name) {
    const ColumnDef* col = table.FindColumn(column_name);
    if (col == nullptr) {
        throw SqlSemanticError("table '" + table.name + "' has no column '" + column_name + "'");
    }
    return *col;
}

void RequireLiteralMatchesColumn(const TableInfo& table, const ColumnDef& col, const LiteralValue& value) {
    bool matches = (col.type == ColumnType::kInteger && value.kind == LiteralValue::Kind::kInteger) ||
                   (col.type == ColumnType::kText && value.kind == LiteralValue::Kind::kString);
    if (!matches) {
        throw SqlSemanticError("column '" + table.name + "." + col.name + "' is " + ColumnTypeToString(col.type) +
                                ", which cannot be compared to a " + LiteralKindName(value.kind) + " literal");
    }
}

size_t ColumnIndex(const TableInfo& table, const std::string& column_name) {
    RequireColumn(table, column_name);
    for (size_t i = 0; i < table.columns.size(); i++) {
        if (table.columns[i].name == column_name) return i;
    }
    throw std::logic_error("ColumnIndex: unreachable -- RequireColumn already confirmed the column exists");
}

}  // namespace flintdb
