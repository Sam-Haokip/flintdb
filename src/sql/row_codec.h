#pragma once
#include "../catalog/catalog.h"
#include "ast.h"

#include <string>
#include <vector>

namespace flintdb {

// Encodes and decodes a table row -- HeapFile only ever sees an opaque
// std::string (storage/heap_file.h); this is where that string's actual
// layout is defined and where the SQL layer (the executor, task #43) sits
// on both sides of it. See docs/DECISIONS.md D-048 for the format and the
// reasoning behind it.
//
// A row's encoding has no NULL bitmap (docs/SPEC.md section 1.1: every
// column is implicitly NOT NULL) and no per-value type tag of its own --
// every caller of DecodeRow already has the table's ColumnDef list on
// hand (from Catalog), which already says each column's type and
// position, so repeating that inside every row would be pure redundancy.
// Values are encoded in `columns`' order, which -- for a row actually
// read back out of a HeapFile -- is always the same `columns` the row was
// encoded with in the first place (a table's schema is fixed for its
// lifetime; docs/SPEC.md section 4 explicitly has no ALTER TABLE).

// Encodes `values` (one per `columns` entry, in order) into the byte
// string HeapFile::Insert stores. Does NOT validate that `values.size()
// == columns.size()` or that each value's kind matches its column's
// type -- that's the executor's job (via sql_semantics.h's
// RequireLiteralMatchesColumn, called once per value before this is ever
// reached), so a bug here fails loud (an out-of-bounds `values[i]` access)
// rather than silently miscoding a mismatched row.
std::string EncodeRow(const std::vector<ColumnDef>& columns, const std::vector<LiteralValue>& values);

// Decodes `row_bytes` (produced by EncodeRow for this same `columns`)
// back into one LiteralValue per column, in table-column order. Throws
// std::runtime_error if `row_bytes` doesn't have enough bytes for
// `columns`' shape -- this should never happen for a row this engine
// itself wrote (EncodeRow/DecodeRow are exact inverses over the same
// schema), so a mismatch here means real corruption or a caller decoding
// against the wrong table's schema, not a routine, recoverable "bad SQL"
// condition -- hence std::runtime_error, not SqlSemanticError (sql_error.h
// is for the user's SQL text/schema resolution, not a raw-bytes integrity
// failure below that layer entirely).
std::vector<LiteralValue> DecodeRow(const std::vector<ColumnDef>& columns, const std::string& row_bytes);

}  // namespace flintdb
