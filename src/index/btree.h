#pragma once
#include "../common/rid.h"
#include "../storage/buffer_pool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flintdb {

// An on-disk B+-tree, keyed by a 64-bit signed integer, mapping each key
// to one or more RIDs (duplicate keys are allowed — see docs/SPEC.md,
// secondary indexes are non-unique). Every node is one page; a "pointer"
// between nodes is a PageId, not a memory address.
//
// Scope note: Phase 2 only needs int64_t keys — that covers an INTEGER
// PRIMARY KEY lookup, which is what this phase's benchmark measures.
// TEXT-keyed indexes are deferred to Phase 5, once the SQL layer defines
// a byte-comparable encoding for TEXT (see docs/DECISIONS.md).
//
// A BPlusTree owns no file of its own — like HeapFile, it's handed a
// BufferPool (and therefore a DiskManager/file) to build on, and in
// Phase 2 that's always a dedicated file, separate from any HeapFile's
// (see docs/DECISIONS.md for why one file per structure, for now).
//
// Thread-safe as of Phase 4, in two distinct senses layered on top of
// each other:
//
// 1. Memory safety: GetRootPageId/SetRootPageId (below) read and write
//    the root pointer through buffer_pool_->FetchPage every time, never
//    caching it in a BPlusTree-local field -- see docs/DECISIONS.md
//    D-042 for why an earlier version of this class *did* cache it in a
//    root_page_id_ member, and why that was a real bug (Transaction::
//    Abort can revert the header page's content via BufferPool::
//    DiscardPage without any way to tell a separate in-memory copy that
//    happened). With no BPlusTree-local copy left to protect, memory
//    safety for the underlying Page* comes entirely from BufferPool's
//    own internal mutex_ -- the same guarantee every other BufferPool
//    consumer already relies on -- so no extra mutex is needed here.
//
// 2. Transactional (isolation) correctness: every public method also
//    acquires locks through GetCurrentTransaction()->AcquireLock (a
//    no-op when no transaction is active, preserving every pre-Phase-4
//    caller's behavior exactly) -- shared for read-only traversals
//    (Search, RangeScanAll, Height), exclusive for every page an
//    Insert/Delete traversal touches, including internal/routing nodes
//    it only reads, not just the leaf it mutates. That's deliberately
//    conservative: a real system would release an ancestor's lock once
//    a child is known not to need to propagate a split/merge back up
//    ("latch crabbing"), allowing two structural changes in unrelated
//    subtrees to proceed concurrently. This project holds every touched
//    page's lock for the whole operation instead (consistent with
//    holding no lock past Strict 2PL's growing phase anyway, and with
//    this project's stated preference for the simpler, provably-correct
//    approach over the more concurrent one -- see docs/SPEC.md section 3
//    and docs/DECISIONS.md).
//
// Memory safety alone (#1) is not sufficient for a correct concurrent
// B-tree: page-level locks on individual *nodes* don't, on their own,
// protect against a transaction reading a stale root_page_id_ moments
// before a concurrent Insert splits the root and replaces it -- the
// stale traversal would silently miss whatever moved into the new
// sibling, a wrong answer, not a crash, so nothing here would catch it.
// This is fixed by giving "the root pointer as a whole" its own lock
// target: every public method acquires a lock on page 0 of this tree's
// own file -- shared for reads, exclusive for anything that might
// replace root_page_id_ -- for its whole duration, before ever reading
// root_page_id_. Under Strict 2PL (locks held until commit, never
// released early), this means a transaction that has read
// root_page_id_ is guaranteed no concurrent transaction can be in the
// middle of changing it, and vice versa: a transaction restructuring
// the root holds page 0's lock exclusively until it commits, so nobody
// else can read (or write) root_page_id_ out from under it. The cost is
// that Insert/Delete/Search/RangeScanAll/Height on *the same BPlusTree*
// end up serialized against each other at the root, even when their
// actual page-level work never overlaps -- a real system would use a
// finer-grained scheme (e.g. an optimistic root-generation counter, or
// treating the root pointer as just another node reached via latch
// crabbing); this project accepts the coarser, simpler, still-correct
// alternative, the same trade this project makes throughout Phase 4 (see
// the class comment above and docs/DECISIONS.md).
//
// Why page 0 specifically, and not a synthetic non-page id (which is
// what an earlier version of this design used): as of Phase 5
// (docs/DECISIONS.md D-036), page 0 of this tree's own file is reserved
// to durably *store* root_page_id_ (see the constructor and
// SetRootPageId in btree.cpp) -- root_page_id_ was, before D-036, never
// actually persisted at all, a real Phase 2 gap that no test caught
// because no Phase 2-4 test ever closed and reopened a BPlusTree over
// the same file. Once the root pointer's storage is a real page, locking
// that real page *is* locking the root pointer -- no separate synthetic
// id is needed, and mutating it goes through the ordinary
// BufferPool::MarkDirty path, which makes it durable and crash-recoverable
// via the existing WAL/recovery machinery for free, with no bespoke
// persistence mechanism of its own.
//
// ValidateInvariants() is the one exception: it's explicitly
// debug/test-only (see its own comment) and does not acquire any
// LockManager lock for its initial read of the root pointer, relying
// only on the memory safety GetRootPageId() already provides via
// BufferPool (point #1 above) -- it is not part of the transactional
// read/write API surface this class otherwise provides.
class BPlusTree {
 public:
    // `object_id` (Phase 5, docs/DECISIONS.md D-031/D-032) identifies
    // this index's own file within a shared LockManager/WAL -- see
    // HeapFile's constructor comment (storage/heap_file.h) for the full
    // rationale, which applies identically here.
    //
    // If `buffer_pool`'s underlying file is not empty (i.e. this is
    // reopening an existing index file), page 0 is assumed to already be
    // this tree's header page (see the class comment on D-036) -- nothing
    // needs to be read back from it here, since GetRootPageId() (below)
    // reads it fresh on every call rather than caching it at construction
    // time (docs/DECISIONS.md D-042). Otherwise (a brand-new file), page 0
    // is freshly allocated and initialized as an empty tree's header. This
    // must be the very first page ever touched on `buffer_pool` for a
    // fresh file, so that page 0 is guaranteed to be the header and never
    // collide with a real leaf/internal node's id.
    BPlusTree(ObjectId object_id, BufferPool* buffer_pool);

    void Insert(int64_t key, RID rid);

    // Returns every RID stored under `key` (empty if none).
    std::vector<RID> Search(int64_t key) const;

    // Removes exactly the (key, rid) entry — both must match, since a key
    // can have several RIDs under it. Returns false if no such entry
    // exists.
    bool Delete(int64_t key, RID rid);

    bool Empty() const;

    // Number of levels from root to leaf, inclusive (1 for a tree that's
    // just a single leaf). 0 for an empty tree. Mainly for tests/
    // diagnostics.
    size_t Height() const;

    // Every (key, RID) pair in ascending key order (ties broken by
    // insertion-independent leaf order). For tests, debugging, and future
    // range-scan support — walks the leaf chain via each leaf's
    // next-leaf pointer.
    std::vector<std::pair<int64_t, RID>> RangeScanAll() const;

    // Debug/test-only structural check: walks the whole tree and confirms
    // every *correctness* invariant holds -- children[i] holds keys in
    // [keys[i-1], keys[i]), leaf/internal keys sorted, child-count ==
    // key-count+1, no node over its max size. RangeScanAll/Search agreeing
    // with a model can't catch a routing violation that only strands a
    // duplicate key unreachable without a model that happens to probe
    // exactly that key -- this checks the structure directly. Deliberately
    // does not check the min-fill target (see ValidateSubtree's comment in
    // btree.cpp for why that's a soft target, not a hard invariant, with
    // duplicate keys). Returns nullopt if the tree is well-formed, or a
    // message describing the first violation found.
    std::optional<std::string> ValidateInvariants() const;

 private:
    struct SplitResult {
        int64_t split_key;
        PageId new_right_page_id;
    };
    struct DeleteResult {
        bool found = false;
        bool underflow = false;
    };

    std::optional<SplitResult> InsertRecursive(PageId page_id, int64_t key, RID rid);
    DeleteResult DeleteRecursive(PageId page_id, bool is_root, int64_t key, RID rid);

    // Rebalances `parent`'s child at `idx` after that child underflowed,
    // by borrowing from a sibling or merging with one. Mutates `parent`
    // in place (its own serialization is the caller's job); writes out
    // whatever sibling/child pages it touches itself.
    void FixUnderflow(struct InternalNode& parent, size_t idx);

    // Reads/writes the root pointer directly through the header page
    // (page 0, D-036) on every call -- see the class comment's point #1
    // and docs/DECISIONS.md D-042 for why this is deliberately *not*
    // cached in a BPlusTree-local field. Every caller of GetRootPageId
    // still needs its own *transactional* lock on page 0 (see btree.cpp's
    // LockRoot) acquired beforehand; these two do not acquire that lock
    // themselves, since ValidateInvariants() deliberately uses the
    // memory-safe read without the transactional one. SetRootPageId
    // persists the new value via BufferPool::MarkDirty (D-036) -- every
    // caller already holds page 0's lock exclusively before calling it,
    // for the reason explained in the class comment.
    PageId GetRootPageId() const;
    void SetRootPageId(PageId page_id);

    ObjectId object_id_;
    BufferPool* buffer_pool_;
};

}  // namespace flintdb
