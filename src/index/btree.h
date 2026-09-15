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
class BPlusTree {
 public:
    explicit BPlusTree(BufferPool* buffer_pool);

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

    BufferPool* buffer_pool_;
    PageId root_page_id_ = INVALID_PAGE_ID;
};

}  // namespace flintdb
