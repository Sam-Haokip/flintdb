#pragma once
#include <cstddef>
#include <cstdint>

namespace flintdb {

using PageId = uint32_t;
using SlotId = uint16_t;

constexpr PageId INVALID_PAGE_ID = static_cast<PageId>(-1);

// Identifies one table or index (one physical file, one DiskManager +
// BufferPool) within a Phase-5 Database. Assigned once, by the Catalog,
// when the object is created, and fixed for its lifetime -- see
// docs/DECISIONS.md D-031/D-032 for why every object gets its own file
// (rather than sharing one file for the whole database) and why that in
// turn means a bare PageId is no longer globally unique, requiring this
// to qualify both LockManager's lock keys and the WAL's Update records.
using ObjectId = uint32_t;

constexpr ObjectId INVALID_OBJECT_ID = static_cast<ObjectId>(-1);

// Fixed page size for the whole engine. 4096 matches the common OS page
// size, which is the standard choice for this kind of page-based storage
// engine (SQLite defaults to 4096 too) — it keeps a page-sized read/write
// aligned with a typical filesystem block, without us having to justify a
// less conventional number this early. Revisit with a measurement in
// Phase 2 (B-tree fanout) if there's a reason to.
constexpr size_t PAGE_SIZE = 4096;

// Every page's first bytes are laid out the same way regardless of what
// kind of page it is: a PageId, then a 2-byte PageType tag (see below) at
// a fixed offset. What follows the tag is entirely kind-specific — a
// heap page's slotted-record layout (page.h) and a B-tree node's
// key/child arrays (index/btree.h) share nothing beyond this tag, but
// sharing the tag means one function can look at any page's raw bytes
// and know which kind of page it's holding, which matters the moment two
// kinds of page can end up read generically (e.g. Phase 6's corruption
// fuzzing, or just debugging).
enum class PageType : uint16_t {
    kInvalid = 0,
    kHeap = 1,
    kBTreeLeaf = 2,
    kBTreeInternal = 3,
    // Phase 5, docs/DECISIONS.md D-036: page 0 of every BPlusTree's own
    // file is reserved to durably store that tree's root_page_id_ (see
    // index/btree.h/.cpp) -- this tag exists purely so any future
    // generic-page-dispatch code (or a debugging/corruption-fuzzing tool,
    // per this enum's own class comment) can recognize a header page and
    // not mistake it for a leaf/internal node; BPlusTree itself never
    // needs to check this tag, since it always knows page 0 is the header
    // by construction.
    kBTreeHeader = 4,
};

}  // namespace flintdb
