#pragma once
#include <cstddef>
#include <cstdint>

namespace flintdb {

using PageId = uint32_t;
using SlotId = uint16_t;

constexpr PageId INVALID_PAGE_ID = static_cast<PageId>(-1);

// Fixed page size for the whole engine. 4096 matches the common OS page
// size, which is the standard choice for this kind of page-based storage
// engine (SQLite defaults to 4096 too) — it keeps a page-sized read/write
// aligned with a typical filesystem block, without us having to justify a
// less conventional number this early. Revisit with a measurement in
// Phase 2 (B-tree fanout) if there's a reason to.
constexpr size_t PAGE_SIZE = 4096;

}  // namespace flintdb
