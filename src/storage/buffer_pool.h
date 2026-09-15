#pragma once
#include "disk_manager.h"
#include "page.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace flintdb {

// A page cache with the simplest possible policy: no eviction. Every page
// ever touched stays resident in memory for the process's lifetime. This
// is a deliberate Phase-1 simplification (see docs/DECISIONS.md) meant to
// get the storage layer correct and tested before optimizing it — a real
// eviction policy (clock/LRU) is future work once a workload actually
// shows memory pressure, not before.
class BufferPool {
 public:
    explicit BufferPool(DiskManager* disk_manager);

    // Allocates a brand-new, zero-filled page via the DiskManager, caches
    // it dirty, and returns a pointer to the in-memory copy. Writes the
    // new id to *out_page_id if non-null. This is the primitive every
    // page *kind* builds on: it knows nothing about slotted heap pages or
    // B-tree nodes, just raw pages (see common/config.h's PageType for how
    // a page then declares what it actually is).
    Page* NewPage(PageId* out_page_id);

    // Convenience wrapper: NewPage, then initialize it as an empty heap
    // page. Kept separate from NewPage so non-heap page kinds (B-tree
    // nodes, from Phase 2 on) don't have to fight HeapFile-specific setup.
    Page* NewHeapPage(PageId* out_page_id);

    // Returns the in-memory page for `page_id`, reading it from disk into
    // the cache on first access. The returned pointer is owned by the
    // BufferPool and stays valid for the BufferPool's lifetime (no
    // eviction means it never becomes dangling while the pool is alive).
    Page* FetchPage(PageId page_id);

    void MarkDirty(PageId page_id);

    // Writes every dirty page back to disk via the DiskManager.
    void FlushAll();

    size_t NumPagesInMemory() const;
    size_t NumPagesOnDisk() const;

    // Drops every cached page and truncates the underlying file. Used by
    // HeapFile's Phase-1 delete-by-rewrite (see docs/DECISIONS.md).
    void ResetAll();

 private:
    DiskManager* disk_manager_;
    std::unordered_map<PageId, std::unique_ptr<Page>> pages_;
    std::unordered_set<PageId> dirty_;
};

}  // namespace flintdb
