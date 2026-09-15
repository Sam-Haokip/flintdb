#pragma once
#include "config.h"

#include <functional>

namespace flintdb {

// A Record ID identifies exactly one row within one heap file: which page
// it's on, and which slot within that page's slot directory.
//
// Phase-1 caveat (see docs/DECISIONS.md): HeapFile::Delete rewrites the
// whole file, which reassigns every surviving row's RID. A RID is only
// guaranteed stable between calls that don't delete anything. Nothing in
// Phase 1 stores a RID across a delete, so this is safe for now — it will
// need revisiting once Phase 2's B-tree starts storing RIDs as index
// entries.
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
