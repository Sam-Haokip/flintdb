#pragma once
#include "disk_manager.h"
#include "page.h"

#include <functional>
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
//
// No eviction also means every Page* this class ever hands out (via
// NewPage/NewHeapPage/FetchPage) stays valid for the BufferPool's whole
// lifetime — DiscardPage below leans on exactly this to revert a page's
// content in place without invalidating anyone else's pointer to it.
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

    // Registers `observer` to be called with a page's id every time
    // MarkDirty runs for it (including indirectly, via NewPage/
    // NewHeapPage) while it's set. This is how TransactionManager
    // (src/txn) finds out which pages a transaction touched without
    // BufferPool needing to know Transaction/TransactionManager exist —
    // avoids a circular dependency between storage/ and txn/.
    //
    // Exactly one observer can be active at a time, which is all Phase 3
    // needs (only one transaction is ever active — see
    // txn/transaction_manager.h). Multiple concurrent transactions
    // (Phase 4) will need this to become a real registry keyed by which
    // transaction dirtied which page, not a single global callback.
    void SetActiveTransactionObserver(std::function<void(PageId)> observer);
    void ClearActiveTransactionObserver();

    // Reloads `page_id`'s in-memory content from disk, discarding
    // whatever was cached, and clears its dirty flag. Used by
    // Transaction::Abort (txn/transaction_manager.h) to revert a page's
    // in-memory state back to what's durably on disk. A no-op if
    // `page_id` was never cached in the first place.
    //
    // Reloads in place rather than dropping and re-fetching the cache
    // entry, so any Page* another caller is holding for this page_id
    // stays valid and simply reflects the reverted content on its next
    // access — required given no-eviction's stable-pointer guarantee
    // above.
    void DiscardPage(PageId page_id);

 private:
    DiskManager* disk_manager_;
    std::unordered_map<PageId, std::unique_ptr<Page>> pages_;
    std::unordered_set<PageId> dirty_;
    std::function<void(PageId)> active_txn_observer_;
};

}  // namespace flintdb
