#pragma once
#include "../common/rid.h"
#include "buffer_pool.h"

#include <string>
#include <utility>
#include <vector>

namespace flintdb {

// The Phase-1 "trivial baseline" row store: an unordered collection of
// variable-length byte rows, spread across as many pages as needed, with
// no index of any kind — that's Phase 2. A HeapFile owns the *entire*
// underlying file (one table per .db file in Phase 1; multi-table support
// is a Phase 5 concern layered on top).
//
// See docs/SPEC.md for what this layer does and doesn't guarantee, and
// docs/DECISIONS.md for why Delete works the way it does below.
class HeapFile {
 public:
    // If the buffer pool's underlying file already has pages (i.e. this is
    // reopening an existing .db file), those pages are assumed to all
    // belong to this heap file and are picked up automatically.
    explicit HeapFile(BufferPool* buffer_pool);

    RID Insert(const std::string& row_bytes);

    // Returns every live row as (RID, bytes) pairs. Order is page id then
    // slot id — not any particular logical order beyond that.
    std::vector<std::pair<RID, std::string>> Scan() const;

    // Deletes the row at `rid` by rewriting the *entire* heap file without
    // it: every surviving row is re-inserted into a freshly truncated
    // file. Returns false (no-op) if `rid` doesn't currently name a live
    // row.
    //
    // This is deliberately the simplest possible correct implementation —
    // "delete-by-rewrite" is this phase's whole point, per the project
    // brief: no free-list, no in-place reclamation. The real cost is that
    // it reassigns RIDs for every remaining row. That's harmless today
    // (nothing else references a RID across a Delete call yet), but it
    // will need to change once Phase 2's B-tree starts storing RIDs as
    // index entries — logged as an open question in docs/SPEC.md and a
    // decision to revisit in docs/DECISIONS.md.
    bool Delete(RID rid);

    size_t NumRows() const;

 private:
    BufferPool* buffer_pool_;
    std::vector<PageId> page_ids_;
};

}  // namespace flintdb
