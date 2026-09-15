#pragma once
#include "disk_manager.h"
#include "page.h"

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
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
//
// Thread-safe as of Phase 4: a single mutex_ guards pages_, dirty_, and
// the observer registry, held across each public method's whole body.
// This is a *physical latch* protecting the cache's own internals, not
// the *logical lock* that gives transactions isolation — that's
// LockManager's job (src/txn/lock_manager.h), held for a whole
// transaction's lifetime rather than one BufferPool call. The two are
// deliberately separate mechanisms: conflating them would either
// serialize every transaction's page access behind a single pool-wide
// lock (defeating the concurrency LockManager's page-level granularity
// is meant to provide), or risk deadlock between two differently-scoped
// locking systems. Callers are expected to acquire the appropriate
// LockManager lock *before* calling into BufferPool, never the other
// way around, which is exactly what HeapFile/BTree do once Phase 4
// wires locking in (see docs/SPEC.md section 3).
//
// A single pool-wide mutex is coarser than a production buffer pool
// (which would latch per-page or per-partition) -- concurrent
// BufferPool calls for two different pages briefly serialize against
// each other here, including the disk I/O a cold FetchPage/NewPage
// triggers. That's a deliberate scope simplification: what Phase 4's
// concurrency claim actually rests on is that two transactions can hold
// LockManager locks on different pages of the same table *simultaneously
// without blocking each other's lock acquisition* (proven by
// lock_manager_page_level_grants_concurrent_access_to_different_pages_of_the_same_table
// in tests/test_lock_manager.cpp) -- not that BufferPool itself achieves
// lock-free parallel disk I/O, which no part of SPEC.md claims.
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

    // Writes every currently-dirty page back to disk via the
    // DiskManager, regardless of which transaction (if any) dirtied it.
    // Safe only when no transaction is active anywhere in the process --
    // e.g. a checkpoint taken between transactions. Under real Phase 4
    // concurrency, calling this from inside one transaction's Commit
    // would also flush a *different*, still-uncommitted transaction's
    // dirty pages to the data file, violating the no-steal guarantee
    // (docs/SPEC.md section 2) -- see FlushPages below, which
    // TransactionManager::Commit uses instead for exactly this reason.
    void FlushAll();

    // Writes back only the pages named in `page_ids`, if cached and
    // dirty, clearing each one's dirty flag; everything else in the pool
    // is left untouched. This is what TransactionManager::Commit uses
    // (passing that transaction's own DirtiedPages()) so that committing
    // one transaction can never also force a concurrently-active, still
    // -uncommitted transaction's dirty pages to disk -- see FlushAll's
    // comment above for the bug this avoids, and docs/DECISIONS.md for
    // the full writeup of how it was found (during Phase 4 design,
    // before any concurrent code existed to reproduce it against).
    void FlushPages(const std::set<PageId>& page_ids);

    size_t NumPagesInMemory() const;
    size_t NumPagesOnDisk() const;

    // Drops every cached page and truncates the underlying file. Used by
    // HeapFile's Phase-1 delete-by-rewrite (see docs/DECISIONS.md).
    void ResetAll();

    // Registers `observer` to be called with a page's id every time
    // MarkDirty runs for it (including indirectly, via NewPage/
    // NewHeapPage) *from the same thread that registered it*. This is how
    // TransactionManager (src/txn) finds out which pages a transaction
    // touched without BufferPool needing to know Transaction/
    // TransactionManager exist — avoids a circular dependency between
    // storage/ and txn/ (src/txn already depends on src/storage, so the
    // reverse dependency isn't an option).
    //
    // Phase 3 had exactly one observer active at a time (a single global
    // callback), because only one transaction was ever active. Phase 4's
    // thread-per-transaction model generalizes this to a registry keyed
    // by std::this_thread::get_id(): each transaction registers its own
    // observer on Begin (on its own thread) and clears it on Commit/
    // Abort, so MarkDirty routes a dirtied page to whichever transaction
    // -- if any -- is running on the thread that dirtied it, with
    // multiple transactions' observers coexisting correctly since each
    // is keyed by a distinct thread id. A thread with no registered
    // observer (e.g. a non-transactional test calling HeapFile directly)
    // is simply not found in the map, which is the existing
    // "no-op when none is registered" behavior.
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
    // Assumes mutex_ is already held by the caller -- the shared body
    // behind both MarkDirty (which locks) and NewPage (which is already
    // inside its own critical section and must not re-lock a
    // non-recursive std::mutex it already holds).
    void MarkDirtyLocked(PageId page_id);

    DiskManager* disk_manager_;
    std::unordered_map<PageId, std::unique_ptr<Page>> pages_;
    std::unordered_set<PageId> dirty_;
    std::unordered_map<std::thread::id, std::function<void(PageId)>> observers_by_thread_;
    mutable std::mutex mutex_;
};

}  // namespace flintdb
