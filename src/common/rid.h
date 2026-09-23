#pragma once
#include "config.h"

#include <functional>

namespace flintdb {

// A Record ID identifies exactly one row within one heap file: which page
// it's on, and which slot within that page's slot directory.
//
// A RID stays valid across a Delete of some *other* row: HeapFile::Delete
// tombstones a row's slot in place (storage/heap_file.h, docs/DECISIONS.md
// D-015) rather than rewriting the file, so it never reassigns any
// surviving row's RID. (An earlier draft of this comment described the
// original Phase 1 "delete-by-rewrite" behavior, which did reassign every
// surviving RID on every delete — that design was replaced before Phase 2's
// B-tree ever started storing RIDs as index entries, precisely because a
// B-tree entry pointing at a RID that a later delete silently renumbered
// would be a real, hard-to-diagnose correctness hazard.)
struct RID {
    PageId page_id = INVALID_PAGE_ID;
    SlotId slot_id = 0;

    bool operator==(const RID& other) const {
        return page_id == other.page_id && slot_id == other.slot_id;
    }
    bool operator!=(const RID& other) const { return !(*this == other); }
};

}  // namespace flintdb

namespace std {
template <>
struct hash<flintdb::RID> {
    size_t operator()(const flintdb::RID& rid) const noexcept {
        return (static_cast<size_t>(rid.page_id) << 16) ^ static_cast<size_t>(rid.slot_id);
    }
};
}  // namespace std
