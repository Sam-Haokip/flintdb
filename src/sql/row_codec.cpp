#include "row_codec.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace flintdb {

namespace {

void AppendInt64(std::string& out, int64_t v) {
    char buf[sizeof(int64_t)];
    std::memcpy(buf, &v, sizeof(int64_t));
    out.append(buf, sizeof(int64_t));
}

int64_t ReadInt64(const std::string& row_bytes, size_t offset) {
    if (offset + sizeof(int64_t) > row_bytes.size()) {
        throw std::runtime_error("DecodeRow: row_bytes too short for an INTEGER column");
    }
    int64_t v;
    std::memcpy(&v, row_bytes.data() + offset, sizeof(int64_t));
    return v;
}

void AppendUint32(std::string& out, uint32_t v) {
    char buf[sizeof(uint32_t)];
    std::memcpy(buf, &v, sizeof(uint32_t));
    out.append(buf, sizeof(uint32_t));
}

uint32_t ReadUint32(const std::string& row_bytes, size_t offset) {
    if (offset + sizeof(uint32_t) > row_bytes.size()) {
        throw std::runtime_error("DecodeRow: row_bytes too short for a TEXT column's length prefix");
    }
    uint32_t v;
    std::memcpy(&v, row_bytes.data() + offset, sizeof(uint32_t));
    return v;
}

}  // namespace

std::string EncodeRow(const std::vector<ColumnDef>& columns, const std::vector<LiteralValue>& values) {
    std::string out;
    for (size_t i = 0; i < columns.size(); i++) {
        const ColumnDef& col = columns[i];
        const LiteralValue& value = values[i];
        if (col.type == ColumnType::kInteger) {
            AppendInt64(out, value.int_value);
        } else {
            // SPEC section 1.1: TEXT is "variable-length, UTF-8 --
            // length-prefixed on disk". A 4-byte length is more than
            // enough for any row that already has to fit within one
            // PAGE_SIZE (4096-byte) page (HeapFile::Insert rejects a row
            // bigger than that outright), so overflow here is not a
            // real-world concern.
            AppendUint32(out, static_cast<uint32_t>(value.string_value.size()));
            out.append(value.string_value);
        }
    }
    return out;
}

std::vector<LiteralValue> DecodeRow(const std::vector<ColumnDef>& columns, const std::string& row_bytes) {
    std::vector<LiteralValue> values;
    values.reserve(columns.size());
    size_t offset = 0;
    for (const ColumnDef& col : columns) {
        LiteralValue value;
        if (col.type == ColumnType::kInteger) {
            value.kind = LiteralValue::Kind::kInteger;
            value.int_value = ReadInt64(row_bytes, offset);
            offset += sizeof(int64_t);
        } else {
            value.kind = LiteralValue::Kind::kString;
            uint32_t len = ReadUint32(row_bytes, offset);
            offset += sizeof(uint32_t);
            if (offset + len > row_bytes.size()) {
                throw std::runtime_error("DecodeRow: row_bytes too short for a TEXT column's declared length");
            }
            value.string_value = row_bytes.substr(offset, len);
            offset += len;
        }
        values.push_back(std::move(value));
    }
    return values;
}

}  // namespace flintdb
