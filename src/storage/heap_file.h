#pragma once
#include "../common/rid.h"
#include "buffer_pool.h"

#include <functional>
#include <mutex>
#include <optional>
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
//
// Thread-safe as of Phase 4, in the same two senses BPlusTree is (see
// btree.h's class comment for the general pattern):
//
// 1. Memory safety: page_ids_mutex_ guards page_ids_ itself, plain
//    shared mutable state outside of any page -- Insert can append to it
//    (a new page) and Scan/Find/Delete/NumRows all read it, none of
//    which is safe to do concurrently, unsynchronized, on a
//    std::vector. Always held only briefly (a snapshot copy, or a single
//    push_back/find), never across a BufferPool call or a LockManager
//    lock acquisition that could block -- consistent with every other
//    latch in this codebase never being held across a call that can
//    wait on something else.
//
// 2. Transactional (isolation) correctness: every public method acquires
//    a LockManager lock through GetCurrentTransaction()->AcquireLock (a
//    no-op with no active transaction, preserving every pre-Phase-4
//    caller's behavior exactly) for each page it touches -- shared for
//    Scan/Find, exclusive for Insert/Delete. Unlike BPlusTree, HeapFile
//    has no equivalent of a "root pointer" that a stale read could
//    silently miscompute from: page_ids_ only ever grows (Insert appends,
//    nothing ever removes a page from it), so a transaction that
//    snapshots page_ids_ before a concurrent Insert appends a new page
//    simply doesn't see that new page in *this* Scan/Find call, no
//    different in kind from a transaction that committed a fraction of a
//    second later under Strict 2PL -- not a correctness gap.
class HeapFile {
 public:
    // If the buffer pool's underlying file already has pages (i.e. this is
    // reopening an existing .db file), those pages are assumed to all
    // belong to this heap file and are picked up automatically.
    //
    // `object_id` (Phase 5, docs/DECISIONS.md D-031/D-032) identifies
    // this heap file's own table within a shared LockManager/WAL: every
    // LockManager::AcquireLock call this class makes is keyed by
    // (object_id, page_id), not a bare page_id, since Phase 5 lets many
    // tables/indexes -- each with page ids starting back at 0 in their
    // own file -- share one LockManager. A HeapFile constructed directly
    // (as every pre-Phase-5 test still does) rather than through the
    // Database facade can pass any fixed id (0 is the convention this
    // codebase's own tests use) as long as it's unique among whatever
    // other objects share the same LockManager/TransactionManager in that
    // scope -- see transaction_manager.h's RegisterObject.
    HeapFile(ObjectId object_id, BufferPool* buffer_pool);

    RID Insert(const std::string& row_bytes);

    // Returns the row's bytes at `rid`, or std::nullopt if `rid` doesn't
    // currently name a live row (unknown page, out-of-range slot, or
    // already-deleted slot -- the same "not found" cases Delete below
    // already distinguishes). Added in Phase 5 (docs/DECISIONS.md D-048)
    // for the executor's index-scan access path: a BPlusTree::Search
    // returns RIDs, and this is the only way to turn one back into an
    // actual row's bytes -- Scan()/Find() alone can't do it, since
    // neither takes a specific RID to look up.
    std::optional<std::string> GetRow(RID rid) const;

    // Returns every live row as (RID, bytes) pairs. Order is page id then
    // slot id — not any particular logical order beyond that.
    std::vector<std::pair<RID, std::string>> Scan() const;

    // Scans rows in order, calling `pred` on each, and returns the RID of
    // the first row for which it returns true — stopping immediately,
    // unlike Scan(), which always materializes every row. This is what a
    // real sequential-scan WHERE-clause lookup does, and it's what Phase
    // 2's scan-vs-index benchmark needs in order to be a fair comparison
    // rather than a strawman.
    std::optional<RID> Find(const std::function<bool(const std::string&)>& pred) const;

    // Deletes the row at `rid` by tombstoning its slot in place (see
    // Page::DeleteRecord) — no file rewrite, no RID reassignment. Returns
    // false (no-op) if `rid` doesn't currently name a live row (unknown
    // page, out-of-range slot, or already-deleted slot).
    //
    // This replaced the original Phase 1 "delete-by-rewrite" strategy —
    // see docs/DECISIONS.md D-015 for why: a whole-file rewrite reassigns
    // every surviving row's RID, which both Phase 2's B-tree (RIDs stored
    // as index entries) and Phase 3's page-level WAL redo logging (a
    // "this page becomes this" record can't express "the file just got
    // shorter and everything was renumbered") need to not happen.
    bool Delete(RID rid);

    size_t NumRows() const;

 private:
    ObjectId object_id_;
    BufferPool* buffer_pool_;
    std::vector<PageId> page_ids_;
    mutable std::mutex page_ids_mutex_;
};

}  // namespace flintdb
