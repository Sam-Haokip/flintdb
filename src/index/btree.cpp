#include "btree.h"

#include "../txn/transaction.h"

#include <algorithm>
#include <cstring>

namespace flintdb {

// --- On-disk node layout --------------------------------------------------
//
// Both node kinds share the same first bytes as every other page kind in
// the engine (see common/config.h's PageType comment): PageId, then a
// PageType tag. What follows is kind-specific.
//
// Leaf:     [ header(16) | (key:int64, rid.page_id:u32, rid.slot_id:u16)* ]
//           header also carries next_leaf (a PageId, INVALID_PAGE_ID if
//           this is the rightmost leaf) so leaves form a chain for
//           RangeScanAll (and future range queries).
//
// Internal: [ header(16) | child_0:PageId | (key:int64, child_{i+1}:PageId)* ]
//           n keys, n+1 children; children[i] holds keys k with
//           keys[i-1] <= k < keys[i] (keys[-1] = -inf, keys[n] = +inf) --
//           i.e. children[i] is "everything less than keys[i]" among
//           children up to i, found via std::upper_bound.

namespace {

constexpr size_t kOffPageId = 0;     // PageId
constexpr size_t kOffPageType = 4;   // uint16_t
constexpr size_t kOffKeyCount = 6;   // uint16_t
constexpr size_t kOffNextLeaf = 8;   // PageId (leaf only)
constexpr size_t kBTreeHeaderSize = 16;

template <typename T>
T ReadAt(const char* buf, size_t off) {
    T v;
    std::memcpy(&v, buf + off, sizeof(T));
    return v;
}
template <typename T>
void WriteAt(char* buf, size_t off, T v) {
    std::memcpy(buf + off, &v, sizeof(T));
}

PageType ReadPageType(const char* buf) { return static_cast<PageType>(ReadAt<uint16_t>(buf, kOffPageType)); }

// Page 0 of every BPlusTree's own file, reserved (docs/DECISIONS.md
// D-036) to durably store that tree's root_page_id_ and, doubling as
// that, to serve as the LockManager lock target representing "this
// BPlusTree's root pointer" as a whole -- see btree.h's class comment
// for the full rationale (why root_page_id_ needs a lock beyond plain
// per-node locking, and why this is a real page rather than a synthetic
// id as an earlier version of this design used). The BPlusTree
// constructor guarantees this is always the very first page allocated
// on a fresh file, so no real leaf/internal node can ever collide with
// it.
constexpr PageId kRootHeaderPageId = 0;

// Acquires `mode` on (object_id, page_id) through the calling thread's
// current transaction, if any -- a no-op when GetCurrentTransaction() is
// nullptr, which is what keeps every pre-Phase-4 non-transactional
// caller (and every test that calls BPlusTree directly, with no
// TransactionManager involved) working exactly as before, unlocked.
// Qualified by ObjectId as of Phase 5 (docs/DECISIONS.md D-032), since
// page_id alone is only unique within this BPlusTree's own file.
void LockPage(ObjectId object_id, PageId page_id, LockMode mode) {
    if (Transaction* txn = GetCurrentTransaction()) txn->AcquireLock(object_id, page_id, mode);
}

// Shorthand for locking the root header page -- see its own comment
// above and btree.h's class comment for what this protects.
void LockRoot(ObjectId object_id, LockMode mode) { LockPage(object_id, kRootHeaderPageId, mode); }

// Per-entry byte costs, used to compute how many keys fit in one page.
constexpr size_t kLeafEntrySize = sizeof(int64_t) + sizeof(PageId) + sizeof(SlotId);        // 14
constexpr size_t kInternalKeySize = sizeof(int64_t);                                        // 8
constexpr size_t kInternalChildSize = sizeof(PageId);                                       // 4

}  // namespace

// Max/min key counts. Not private BPlusTree members because the
// serialize/deserialize free functions below need them too, and those
// live at file scope (they're the same shape as page.cpp's approach:
// plain functions over raw buffers, not methods on a page-wrapper class,
// since B-tree nodes don't share HeapFile's slotted layout).
constexpr size_t kLeafMaxKeys = (PAGE_SIZE - kBTreeHeaderSize) / kLeafEntrySize;
constexpr size_t kLeafMinKeys = kLeafMaxKeys / 2;
constexpr size_t kInternalMaxKeys =
    (PAGE_SIZE - kBTreeHeaderSize - kInternalChildSize) / (kInternalKeySize + kInternalChildSize);
constexpr size_t kInternalMinKeys = kInternalMaxKeys / 2;

struct LeafNode {
    PageId page_id = INVALID_PAGE_ID;
    PageId next_leaf = INVALID_PAGE_ID;
    std::vector<int64_t> keys;
    std::vector<RID> values;  // values[i] corresponds to keys[i]
};

struct InternalNode {
    PageId page_id = INVALID_PAGE_ID;
    std::vector<int64_t> keys;     // n keys
    std::vector<PageId> children;  // n + 1 children
};

namespace {

void SerializeLeaf(const LeafNode& leaf, char* buf) {
    std::memset(buf, 0, PAGE_SIZE);
    WriteAt<PageId>(buf, kOffPageId, leaf.page_id);
    WriteAt<uint16_t>(buf, kOffPageType, static_cast<uint16_t>(PageType::kBTreeLeaf));
    WriteAt<uint16_t>(buf, kOffKeyCount, static_cast<uint16_t>(leaf.keys.size()));
    WriteAt<PageId>(buf, kOffNextLeaf, leaf.next_leaf);

    size_t off = kBTreeHeaderSize;
    for (size_t i = 0; i < leaf.keys.size(); i++) {
        WriteAt<int64_t>(buf, off, leaf.keys[i]);
        off += sizeof(int64_t);
        WriteAt<PageId>(buf, off, leaf.values[i].page_id);
        off += sizeof(PageId);
        WriteAt<SlotId>(buf, off, leaf.values[i].slot_id);
        off += sizeof(SlotId);
    }
}

LeafNode DeserializeLeaf(const char* buf) {
    LeafNode leaf;
    leaf.page_id = ReadAt<PageId>(buf, kOffPageId);
    leaf.next_leaf = ReadAt<PageId>(buf, kOffNextLeaf);
    uint16_t count = ReadAt<uint16_t>(buf, kOffKeyCount);
    leaf.keys.resize(count);
    leaf.values.resize(count);

    size_t off = kBTreeHeaderSize;
    for (uint16_t i = 0; i < count; i++) {
        leaf.keys[i] = ReadAt<int64_t>(buf, off);
        off += sizeof(int64_t);
        RID r;
        r.page_id = ReadAt<PageId>(buf, off);
        off += sizeof(PageId);
        r.slot_id = ReadAt<SlotId>(buf, off);
        off += sizeof(SlotId);
        leaf.values[i] = r;
    }
    return leaf;
}

void SerializeInternal(const InternalNode& node, char* buf) {
    std::memset(buf, 0, PAGE_SIZE);
    WriteAt<PageId>(buf, kOffPageId, node.page_id);
    WriteAt<uint16_t>(buf, kOffPageType, static_cast<uint16_t>(PageType::kBTreeInternal));
    WriteAt<uint16_t>(buf, kOffKeyCount, static_cast<uint16_t>(node.keys.size()));

    size_t off = kBTreeHeaderSize;
    WriteAt<PageId>(buf, off, node.children[0]);
    off += sizeof(PageId);
    for (size_t i = 0; i < node.keys.size(); i++) {
        WriteAt<int64_t>(buf, off, node.keys[i]);
        off += sizeof(int64_t);
        WriteAt<PageId>(buf, off, node.children[i + 1]);
        off += sizeof(PageId);
    }
}

InternalNode DeserializeInternal(const char* buf) {
    InternalNode node;
    node.page_id = ReadAt<PageId>(buf, kOffPageId);
    uint16_t count = ReadAt<uint16_t>(buf, kOffKeyCount);
    node.keys.resize(count);
    node.children.resize(static_cast<size_t>(count) + 1);

    size_t off = kBTreeHeaderSize;
    node.children[0] = ReadAt<PageId>(buf, off);
    off += sizeof(PageId);
    for (uint16_t i = 0; i < count; i++) {
        node.keys[i] = ReadAt<int64_t>(buf, off);
        off += sizeof(int64_t);
        node.children[i + 1] = ReadAt<PageId>(buf, off);
        off += sizeof(PageId);
    }
    return node;
}

// The root header page's payload: just root_page_id_, stored right after
// the shared 16-byte page header (docs/DECISIONS.md D-036). Distinct from
// SerializeLeaf/SerializeInternal above -- this page is never a tree node,
// just a durable slot for one PageId -- but it still starts with the same
// PageId+PageType tag every page kind shares (see common/config.h's
// PageType comment), tagged kBTreeHeader so nothing generic ever mistakes
// it for a leaf/internal node.
constexpr size_t kOffHeaderRootPageId = kBTreeHeaderSize;

void SerializeHeader(PageId root_page_id, char* buf) {
    std::memset(buf, 0, PAGE_SIZE);
    WriteAt<PageId>(buf, kOffPageId, kRootHeaderPageId);
    WriteAt<uint16_t>(buf, kOffPageType, static_cast<uint16_t>(PageType::kBTreeHeader));
    WriteAt<PageId>(buf, kOffHeaderRootPageId, root_page_id);
}

PageId DeserializeHeaderRootPageId(const char* buf) { return ReadAt<PageId>(buf, kOffHeaderRootPageId); }

// A split's separator key must not be equal to a key left behind on the
// other side of it: the routing invariant every lookup relies on
// (children[i] holds keys in [parent.keys[i-1], parent.keys[i])) demands
// a *strict* boundary. Since keys aren't unique (see docs/SPEC.md --
// secondary indexes are non-unique), a naive size()/2 split can land
// inside a run of equal keys, silently stranding the ones left behind:
// upper_bound-based routing would never visit that side again for that
// key value, even though the entries are still physically present (which
// is exactly why this surfaced as Delete/Search misses rather than a
// RangeScanAll mismatch -- RangeScanAll just walks the leaf chain and
// doesn't use routing at all). Slide the split point to the nearest real
// key-value boundary: forward first, then backward. Returns `naive_mid`
// unchanged only if every key in the array is identical -- a genuine
// pathological case (more duplicates of one key than fit in a single
// page) with no valid boundary to find; see docs/DECISIONS.md.
size_t FindSplitBoundary(const std::vector<int64_t>& keys, size_t naive_mid) {
    for (size_t i = naive_mid; i < keys.size(); i++) {
        if (keys[i] != keys[i - 1]) return i;
    }
    for (size_t i = naive_mid; i >= 1; i--) {
        if (keys[i] != keys[i - 1]) return i;
        if (i == 1) break;
    }
    return naive_mid;
}

// Underflow fix for two leaf siblings (see FixUnderflow) needs the same
// strict-boundary guarantee as a split, but starting from an already-
// small `child` makes a single-entry borrow the wrong unit of work: if
// the borrowed entry's value repeats past the new boundary, there may be
// no valid *single-entry* move that's safe, and searching a whole extra
// leaf just to fall back to an unsafe move (as an earlier version of this
// function did) reintroduces exactly the stranding bug this is meant to
// prevent -- worse, it can strand entries across a leaf boundary in a way
// even a follow-up multi-leaf Search could never find, since Search only
// ever looks in the one leaf routing sends it to. So: concatenate donor
// and recipient into one sorted sequence and pick a split point that
// lands on a real value change, searching outward from `ideal` (the
// caller's fill-factor preference) so the result stays close to it when
// possible. `[min_split, max_split]` bound the search only by the *hard*
// capacity requirement (neither resulting side may exceed kLeafMaxKeys)
// -- deliberately not by kLeafMinKeys, which, same as in FindSplitBoundary,
// can't always be honored at the same time as a safe boundary: kLeafMinKeys
// is exactly kLeafMaxKeys/2, so the min-fill-safe window can be as narrow
// as 2-3 candidate positions, and duplicate-heavy data routinely fills a
// window that size with one repeated value. Widening the search to the
// full capacity-safe range (rather than also stopping at kLeafMinKeys)
// makes that a non-issue in practice; a returned split may leave one side
// under kLeafMinKeys, which ValidateInvariants deliberately doesn't flag
// (see its comment) since it's a fill-factor deviation, not a correctness
// one. Returns nullopt only if every candidate in [min_split, max_split]
// sits inside one run of duplicate keys -- the same genuinely rare
// pathological case FindSplitBoundary documents (more duplicates of one
// key than fit across the two pages involved); the caller falls back to
// `ideal` in that case, same as FindSplitBoundary falling back to
// naive_mid. See docs/DECISIONS.md.
std::optional<size_t> FindRebalanceSplit(const std::vector<int64_t>& keys, size_t min_split, size_t max_split,
                                          size_t ideal) {
    if (ideal < min_split) ideal = min_split;
    if (ideal > max_split) ideal = max_split;
    if (keys[ideal - 1] != keys[ideal]) return ideal;
    for (size_t offset = 1;; offset++) {
        bool in_range = false;
        if (ideal + offset <= max_split) {
            in_range = true;
            if (keys[ideal + offset - 1] != keys[ideal + offset]) return ideal + offset;
        }
        if (offset <= ideal - min_split) {
            in_range = true;
            if (keys[ideal - offset - 1] != keys[ideal - offset]) return ideal - offset;
        }
        if (!in_range) break;
    }
    return std::nullopt;
}

}  // namespace

BPlusTree::BPlusTree(ObjectId object_id, BufferPool* buffer_pool)
    : object_id_(object_id), buffer_pool_(buffer_pool) {
    // No transactional locking needed here, same as HeapFile's
    // constructor: object construction happens before any other thread
    // can possibly have a reference to this BPlusTree.
    if (buffer_pool_->NumPagesOnDisk() == 0) {
        // Fresh file: reserve page 0 as the header, initialized to "empty
        // tree" (docs/DECISIONS.md D-036). This is guaranteed to be the
        // very first page this BPlusTree (or anyone else) ever allocates
        // on this BufferPool, so DiskManager::AllocatePage is guaranteed
        // to hand back id 0 -- every real leaf/internal node allocated
        // afterward is guaranteed *not* to collide with kRootHeaderPageId.
        PageId header_pid;
        Page* header_page = buffer_pool_->NewPage(&header_pid);
        SerializeHeader(INVALID_PAGE_ID, header_page->Data());
        buffer_pool_->MarkDirty(header_pid);
        // Force-flush this one page immediately, before returning from the
        // constructor -- docs/DECISIONS.md D-043. This write happens
        // outside of any transaction (object creation is always DDL,
        // D-033), so nothing else will ever flush it on this object's
        // behalf; left pending, it would get silently folded into
        // whichever transaction happens to touch this page *next* (e.g.
        // the very first Insert, via SetRootPageId), and an abort of that
        // transaction would then revert this page all the way back to
        // raw, never-written bytes instead of the "empty tree" sentinel
        // that should always be there. FlushPages (not FlushAll) to name
        // exactly the one page this call is responsible for, the same
        // scoping reasoning as D-027.
        buffer_pool_->FlushPages({header_pid});
    }
    // Reopening an existing file: the header page (page 0) is already
    // there, but nothing needs to happen here -- GetRootPageId() (below)
    // reads it fresh from buffer_pool_ on every call rather than caching
    // it in a member at construction time (docs/DECISIONS.md D-042), so
    // there's nothing to read back eagerly. An earlier version of this
    // constructor did an eager FetchPage+cache here as the original fix
    // for D-036's "root pointer never persisted" gap; D-042 replaced that
    // cache with a live read after finding a case (Transaction::Abort
    // reverting the header page via BufferPool::DiscardPage) where the
    // cached copy and the real page content could disagree.
}

PageId BPlusTree::GetRootPageId() const {
    Page* header_page = buffer_pool_->FetchPage(kRootHeaderPageId);
    return DeserializeHeaderRootPageId(header_page->Data());
}

void BPlusTree::SetRootPageId(PageId page_id) {
    // Persist immediately, via the same MarkDirty path every other page
    // mutation in this codebase goes through -- this is what makes the
    // root pointer durable and crash-recoverable via the ordinary
    // WAL/recovery machinery for free (docs/DECISIONS.md D-036), with no
    // bespoke persistence mechanism needed, and it's the *only* copy of
    // this value that exists (docs/DECISIONS.md D-042) -- there is no
    // separate in-memory field this could leave out of sync. Safe to do
    // with no BPlusTree-local locking (BufferPool::FetchPage/MarkDirty
    // are already internally synchronized) because every caller of
    // SetRootPageId already holds page 0's LockManager lock exclusively
    // for the whole operation -- see btree.h's class comment.
    Page* header_page = buffer_pool_->FetchPage(kRootHeaderPageId);
    SerializeHeader(page_id, header_page->Data());
    buffer_pool_->MarkDirty(kRootHeaderPageId);
}

bool BPlusTree::Empty() const {
    // A single-field read with no multi-step traversal to protect from a
    // concurrent root change -- GetRootPageId()'s own memory safety
    // (docs/DECISIONS.md D-042) is enough here, no need for the
    // transactional sentinel lock too.
    return GetRootPageId() == INVALID_PAGE_ID;
}

size_t BPlusTree::Height() const {
    LockRoot(object_id_, LockMode::kShared);
    PageId root = GetRootPageId();
    if (root == INVALID_PAGE_ID) return 0;
    size_t h = 1;
    PageId cur = root;
    LockPage(object_id_, cur, LockMode::kShared);
    Page* page = buffer_pool_->FetchPage(cur);
    while (ReadPageType(page->Data()) != PageType::kBTreeLeaf) {
        InternalNode node = DeserializeInternal(page->Data());
        cur = node.children.front();
        LockPage(object_id_, cur, LockMode::kShared);
        page = buffer_pool_->FetchPage(cur);
        h++;
    }
    return h;
}

std::vector<RID> BPlusTree::Search(int64_t key) const {
    std::vector<RID> results;
    LockRoot(object_id_, LockMode::kShared);
    PageId root = GetRootPageId();
    if (root == INVALID_PAGE_ID) return results;

    PageId cur = root;
    while (true) {
        LockPage(object_id_, cur, LockMode::kShared);
        Page* page = buffer_pool_->FetchPage(cur);
        if (ReadPageType(page->Data()) == PageType::kBTreeLeaf) {
            LeafNode leaf = DeserializeLeaf(page->Data());
            for (size_t i = 0; i < leaf.keys.size(); i++) {
                if (leaf.keys[i] == key) results.push_back(leaf.values[i]);
            }
            return results;
        }
        InternalNode node = DeserializeInternal(page->Data());
        size_t idx = static_cast<size_t>(std::upper_bound(node.keys.begin(), node.keys.end(), key) - node.keys.begin());
        cur = node.children[idx];
    }
}

std::vector<std::pair<int64_t, RID>> BPlusTree::RangeScanAll() const {
    std::vector<std::pair<int64_t, RID>> out;
    LockRoot(object_id_, LockMode::kShared);
    PageId root = GetRootPageId();
    if (root == INVALID_PAGE_ID) return out;

    PageId cur = root;
    LockPage(object_id_, cur, LockMode::kShared);
    Page* page = buffer_pool_->FetchPage(cur);
    while (ReadPageType(page->Data()) != PageType::kBTreeLeaf) {
        InternalNode node = DeserializeInternal(page->Data());
        cur = node.children.front();
        LockPage(object_id_, cur, LockMode::kShared);
        page = buffer_pool_->FetchPage(cur);
    }
    // `cur`/`page` are now the leftmost leaf, already locked and fetched.
    while (true) {
        LeafNode leaf = DeserializeLeaf(page->Data());
        for (size_t i = 0; i < leaf.keys.size(); i++) out.emplace_back(leaf.keys[i], leaf.values[i]);
        if (leaf.next_leaf == INVALID_PAGE_ID) break;
        cur = leaf.next_leaf;
        LockPage(object_id_, cur, LockMode::kShared);
        page = buffer_pool_->FetchPage(cur);
    }
    return out;
}

namespace {

// Recursive helper for ValidateInvariants. `lower`/`has_lower` and
// `upper`/`has_upper` are the bounds this subtree's keys must fall in,
// inherited from the parent separator(s) that route to it (open on the
// low end at the very left spine of the tree, open on the high end at
// the very right spine).
//
// Deliberately does NOT check the kLeafMinKeys/kInternalMinKeys lower
// bound. That's a fill-factor target, not a correctness requirement, and
// with duplicate keys it cannot always be met: kLeafMinKeys is exactly
// kLeafMaxKeys/2, leaving only one entry of slack in a fresh split, so a
// long run of equal keys can force FindSplitBoundary's search away from
// the midpoint far enough to leave one side under kLeafMinKeys (see its
// comment, and docs/DECISIONS.md). That leaf is still fully correct --
// every key is still reachable, nothing is stranded -- it just holds
// fewer entries than the target until it's next touched by a delete,
// at which point normal underflow handling picks it up regardless of how
// far under the target it started. Routing-bound compliance (checked
// below) is the actual reachability guarantee this function exists to
// verify.
std::optional<std::string> ValidateSubtree(BufferPool* bp, PageId page_id, bool has_lower, int64_t lower,
                                            bool has_upper, int64_t upper) {
    Page* page = bp->FetchPage(page_id);
    if (ReadPageType(page->Data()) == PageType::kBTreeLeaf) {
        LeafNode leaf = DeserializeLeaf(page->Data());
        if (leaf.keys.size() > kLeafMaxKeys) {
            return "leaf " + std::to_string(page_id) + " overflowed: " + std::to_string(leaf.keys.size()) +
                   " keys > max " + std::to_string(kLeafMaxKeys);
        }
        for (size_t i = 0; i < leaf.keys.size(); i++) {
            if (i > 0 && leaf.keys[i] < leaf.keys[i - 1]) {
                return "leaf " + std::to_string(page_id) + " keys not sorted at index " + std::to_string(i);
            }
            if (has_lower && leaf.keys[i] < lower) {
                return "leaf " + std::to_string(page_id) + " key " + std::to_string(leaf.keys[i]) +
                       " at index " + std::to_string(i) + " violates lower bound " + std::to_string(lower);
            }
            if (has_upper && leaf.keys[i] >= upper) {
                return "leaf " + std::to_string(page_id) + " key " + std::to_string(leaf.keys[i]) +
                       " at index " + std::to_string(i) + " violates upper bound " + std::to_string(upper);
            }
        }
        return std::nullopt;
    }

    InternalNode node = DeserializeInternal(page->Data());
    if (node.keys.size() > kInternalMaxKeys) {
        return "internal " + std::to_string(page_id) + " overflowed: " + std::to_string(node.keys.size()) +
               " keys > max " + std::to_string(kInternalMaxKeys);
    }
    if (node.children.size() != node.keys.size() + 1) {
        return "internal " + std::to_string(page_id) + " has " + std::to_string(node.children.size()) +
               " children but " + std::to_string(node.keys.size()) + " keys";
    }
    for (size_t i = 1; i < node.keys.size(); i++) {
        if (node.keys[i] < node.keys[i - 1]) {
            return "internal " + std::to_string(page_id) + " keys not sorted at index " + std::to_string(i);
        }
    }
    for (size_t i = 0; i < node.children.size(); i++) {
        bool child_has_lower = (i == 0) ? has_lower : true;
        int64_t child_lower = (i == 0) ? lower : node.keys[i - 1];
        bool child_has_upper = (i == node.children.size() - 1) ? has_upper : true;
        int64_t child_upper = (i == node.children.size() - 1) ? upper : node.keys[i];
        auto err = ValidateSubtree(bp, node.children[i], child_has_lower, child_lower, child_has_upper, child_upper);
        if (err.has_value()) return err;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> BPlusTree::ValidateInvariants() const {
    // Deliberately GetRootPageId() (memory-safe) rather than the
    // transactional LockRoot()+lock -- see the class comment: this is
    // debug/test-only and not part of the transactional API surface.
    PageId root = GetRootPageId();
    if (root == INVALID_PAGE_ID) return std::nullopt;
    return ValidateSubtree(buffer_pool_, root, /*has_lower=*/false, 0, /*has_upper=*/false, 0);
}

void BPlusTree::Insert(int64_t key, RID rid) {
    // Exclusive on the whole operation: Insert may replace root_page_id_
    // (on a root split), so it needs the same protection a Delete-side
    // root collapse does -- see btree.h's class comment on
    // kRootLockSentinel.
    LockRoot(object_id_, LockMode::kExclusive);
    PageId root = GetRootPageId();

    if (root == INVALID_PAGE_ID) {
        PageId pid;
        Page* page = buffer_pool_->NewPage(&pid);
        LockPage(object_id_, pid, LockMode::kExclusive);  // bookkeeping only -- a brand-new page can't conflict with anyone
        LeafNode leaf;
        leaf.page_id = pid;
        leaf.keys.push_back(key);
        leaf.values.push_back(rid);
        SerializeLeaf(leaf, page->Data());
        buffer_pool_->MarkDirty(pid);
        SetRootPageId(pid);
        return;
    }

    auto split = InsertRecursive(root, key, rid);
    if (split.has_value()) {
        PageId new_root_pid;
        Page* new_root_page = buffer_pool_->NewPage(&new_root_pid);
        LockPage(object_id_, new_root_pid, LockMode::kExclusive);
        InternalNode new_root;
        new_root.page_id = new_root_pid;
        new_root.keys.push_back(split->split_key);
        new_root.children.push_back(root);
        new_root.children.push_back(split->new_right_page_id);
        SerializeInternal(new_root, new_root_page->Data());
        buffer_pool_->MarkDirty(new_root_pid);
        SetRootPageId(new_root_pid);
    }
}

std::optional<BPlusTree::SplitResult> BPlusTree::InsertRecursive(PageId page_id, int64_t key, RID rid) {
    LockPage(object_id_, page_id, LockMode::kExclusive);
    Page* page = buffer_pool_->FetchPage(page_id);

    if (ReadPageType(page->Data()) == PageType::kBTreeLeaf) {
        LeafNode leaf = DeserializeLeaf(page->Data());
        size_t pos = static_cast<size_t>(std::upper_bound(leaf.keys.begin(), leaf.keys.end(), key) - leaf.keys.begin());
        leaf.keys.insert(leaf.keys.begin() + static_cast<long>(pos), key);
        leaf.values.insert(leaf.values.begin() + static_cast<long>(pos), rid);

        if (leaf.keys.size() <= kLeafMaxKeys) {
            SerializeLeaf(leaf, page->Data());
            buffer_pool_->MarkDirty(page_id);
            return std::nullopt;
        }

        // Overflowed: split. Left keeps the first half, right gets the
        // rest; the right half's first key is copied up as the separator
        // (leaves keep their own copy of every key, unlike internal
        // splits which push the middle key up and remove it below).
        size_t mid = FindSplitBoundary(leaf.keys, leaf.keys.size() / 2);
        LeafNode right;
        PageId right_pid;
        Page* right_page = buffer_pool_->NewPage(&right_pid);
        LockPage(object_id_, right_pid, LockMode::kExclusive);  // bookkeeping only -- brand new
        right.page_id = right_pid;
        right.keys.assign(leaf.keys.begin() + static_cast<long>(mid), leaf.keys.end());
        right.values.assign(leaf.values.begin() + static_cast<long>(mid), leaf.values.end());
        right.next_leaf = leaf.next_leaf;

        leaf.keys.resize(mid);
        leaf.values.resize(mid);
        leaf.next_leaf = right_pid;

        SerializeLeaf(leaf, page->Data());
        SerializeLeaf(right, right_page->Data());
        buffer_pool_->MarkDirty(page_id);
        buffer_pool_->MarkDirty(right_pid);

        return SplitResult{right.keys.front(), right_pid};
    }

    InternalNode node = DeserializeInternal(page->Data());
    size_t idx = static_cast<size_t>(std::upper_bound(node.keys.begin(), node.keys.end(), key) - node.keys.begin());
    auto child_split = InsertRecursive(node.children[idx], key, rid);
    if (!child_split.has_value()) return std::nullopt;

    // Re-fetch: recursion may have allocated new pages and grown the
    // buffer pool's map, but never invalidates a Page* (pages are
    // unique_ptr-owned and stable), so `page` above is still valid — this
    // re-fetch is just for clarity, not correctness.
    node.keys.insert(node.keys.begin() + static_cast<long>(idx), child_split->split_key);
    node.children.insert(node.children.begin() + static_cast<long>(idx) + 1, child_split->new_right_page_id);

    if (node.keys.size() <= kInternalMaxKeys) {
        SerializeInternal(node, page->Data());
        buffer_pool_->MarkDirty(page_id);
        return std::nullopt;
    }

    // Overflowed: split, pushing the middle key up (it appears in the
    // parent only, not in either half below). Same duplicate-boundary
    // concern as the leaf split above applies here too.
    size_t mid = FindSplitBoundary(node.keys, node.keys.size() / 2);
    int64_t pushed_up = node.keys[mid];

    InternalNode right;
    PageId right_pid;
    Page* right_page = buffer_pool_->NewPage(&right_pid);
    LockPage(object_id_, right_pid, LockMode::kExclusive);  // bookkeeping only -- brand new
    right.page_id = right_pid;
    right.keys.assign(node.keys.begin() + static_cast<long>(mid) + 1, node.keys.end());
    right.children.assign(node.children.begin() + static_cast<long>(mid) + 1, node.children.end());

    node.keys.resize(mid);
    node.children.resize(mid + 1);

    SerializeInternal(node, page->Data());
    SerializeInternal(right, right_page->Data());
    buffer_pool_->MarkDirty(page_id);
    buffer_pool_->MarkDirty(right_pid);

    return SplitResult{pushed_up, right_pid};
}

bool BPlusTree::Delete(int64_t key, RID rid) {
    // Exclusive on the whole operation: Delete may replace root_page_id_
    // on a root collapse -- see btree.h's class comment.
    LockRoot(object_id_, LockMode::kExclusive);
    PageId root = GetRootPageId();
    if (root == INVALID_PAGE_ID) return false;

    DeleteResult result = DeleteRecursive(root, /*is_root=*/true, key, rid);
    if (!result.found) return false;

    // Root collapse: a leaf root can become empty, or an internal root
    // can be merged down to a single child, in either of which cases the
    // tree shrinks by one level (or to nothing). `root` is already
    // exclusively locked (DeleteRecursive locked it on the way in), so
    // no additional LockPage call is needed for this re-fetch.
    Page* root_page = buffer_pool_->FetchPage(root);
    if (ReadPageType(root_page->Data()) == PageType::kBTreeLeaf) {
        LeafNode root_leaf = DeserializeLeaf(root_page->Data());
        if (root_leaf.keys.empty()) {
            SetRootPageId(INVALID_PAGE_ID);
        }
    } else {
        InternalNode root_internal = DeserializeInternal(root_page->Data());
        if (root_internal.children.size() == 1) {
            SetRootPageId(root_internal.children[0]);
        }
    }
    return true;
}

BPlusTree::DeleteResult BPlusTree::DeleteRecursive(PageId page_id, bool is_root, int64_t key, RID rid) {
    LockPage(object_id_, page_id, LockMode::kExclusive);
    Page* page = buffer_pool_->FetchPage(page_id);

    if (ReadPageType(page->Data()) == PageType::kBTreeLeaf) {
        LeafNode leaf = DeserializeLeaf(page->Data());
        size_t pos = leaf.keys.size();
        for (size_t i = 0; i < leaf.keys.size(); i++) {
            if (leaf.keys[i] == key && leaf.values[i] == rid) {
                pos = i;
                break;
            }
        }
        if (pos == leaf.keys.size()) return {false, false};

        leaf.keys.erase(leaf.keys.begin() + static_cast<long>(pos));
        leaf.values.erase(leaf.values.begin() + static_cast<long>(pos));
        SerializeLeaf(leaf, page->Data());
        buffer_pool_->MarkDirty(page_id);

        bool underflow = !is_root && leaf.keys.size() < kLeafMinKeys;
        return {true, underflow};
    }

    InternalNode node = DeserializeInternal(page->Data());
    size_t idx = static_cast<size_t>(std::upper_bound(node.keys.begin(), node.keys.end(), key) - node.keys.begin());
    DeleteResult child_result = DeleteRecursive(node.children[idx], false, key, rid);
    if (!child_result.found) return {false, false};

    if (child_result.underflow) {
        FixUnderflow(node, idx);
    }

    SerializeInternal(node, page->Data());
    buffer_pool_->MarkDirty(page_id);

    bool underflow = !is_root && node.keys.size() < kInternalMinKeys;
    return {true, underflow};
}

void BPlusTree::FixUnderflow(InternalNode& parent, size_t idx) {
    PageId child_pid = parent.children[idx];
    // Already locked by the DeleteRecursive call that led here (this
    // page id is exactly the one it just finished processing) -- calling
    // LockPage again is a harmless no-op, kept for clarity that this
    // function's own contract requires it regardless of caller.
    LockPage(object_id_, child_pid, LockMode::kExclusive);
    Page* child_page = buffer_pool_->FetchPage(child_pid);
    bool has_left = idx > 0;
    bool has_right = idx + 1 < parent.children.size();

    if (ReadPageType(child_page->Data()) == PageType::kBTreeLeaf) {
        LeafNode child = DeserializeLeaf(child_page->Data());

        // Fix by merging with a sibling when the combined data fits in one
        // page (classic case, and always exactly what happens when the
        // sibling has no spare capacity to redistribute instead — combined
        // size is then <= 2*kLeafMinKeys - 1 < kLeafMaxKeys, guaranteed to
        // fit). Otherwise redistribute: rather than move a fixed number of
        // entries and hope the resulting boundary is duplicate-safe (an
        // earlier version of this function did that and could both strand
        // entries across a leaf boundary that single-leaf Search() can
        // never find there again, *and*, in a different earlier version,
        // overflow the recipient page outright -- see docs/DECISIONS.md),
        // treat the two siblings as one combined sorted sequence and pick
        // a split point that (a) keeps both halves within kLeafMaxKeys and
        // (b) lands on an actual value change, via FindRebalanceSplit (see
        // its comment for why (a) is capacity-only, not also
        // kLeafMinKeys). A non-root node always has at least one sibling,
        // so exactly one of the two blocks below runs.
        if (has_left) {
            LockPage(object_id_, parent.children[idx - 1], LockMode::kExclusive);
            Page* left_page = buffer_pool_->FetchPage(parent.children[idx - 1]);
            LeafNode left = DeserializeLeaf(left_page->Data());
            size_t combined_size = left.keys.size() + child.keys.size();

            if (combined_size <= kLeafMaxKeys) {
                left.keys.insert(left.keys.end(), child.keys.begin(), child.keys.end());
                left.values.insert(left.values.end(), child.values.begin(), child.values.end());
                left.next_leaf = child.next_leaf;
                SerializeLeaf(left, left_page->Data());
                buffer_pool_->MarkDirty(parent.children[idx - 1]);
                parent.keys.erase(parent.keys.begin() + static_cast<long>(idx) - 1);
                parent.children.erase(parent.children.begin() + static_cast<long>(idx));
                return;
            }

            std::vector<int64_t> keys = left.keys;
            keys.insert(keys.end(), child.keys.begin(), child.keys.end());
            std::vector<RID> values = left.values;
            values.insert(values.end(), child.values.begin(), child.values.end());

            // Bounds enforce ONLY the hard capacity constraint (neither
            // side may exceed kLeafMaxKeys) -- not the kLeafMinKeys target,
            // which, same as in FindSplitBoundary, cannot always be met
            // simultaneously with a safe boundary (see FindRebalanceSplit's
            // comment). `ideal` still aims for the fill-factor target; the
            // search just isn't confined to it.
            size_t min_split = combined_size - kLeafMaxKeys;
            size_t max_split = kLeafMaxKeys;
            size_t ideal = combined_size - kLeafMinKeys;  // leave left as full as possible, like a minimal borrow
            size_t split_at = FindRebalanceSplit(keys, min_split, max_split, ideal)
                                   .value_or(std::clamp(ideal, min_split, max_split));

            left.keys.assign(keys.begin(), keys.begin() + static_cast<long>(split_at));
            left.values.assign(values.begin(), values.begin() + static_cast<long>(split_at));
            child.keys.assign(keys.begin() + static_cast<long>(split_at), keys.end());
            child.values.assign(values.begin() + static_cast<long>(split_at), values.end());
            parent.keys[idx - 1] = child.keys.front();
            SerializeLeaf(left, left_page->Data());
            SerializeLeaf(child, child_page->Data());
            buffer_pool_->MarkDirty(parent.children[idx - 1]);
            buffer_pool_->MarkDirty(child_pid);
            return;
        }

        LockPage(object_id_, parent.children[idx + 1], LockMode::kExclusive);
        Page* right_page = buffer_pool_->FetchPage(parent.children[idx + 1]);
        LeafNode right = DeserializeLeaf(right_page->Data());
        size_t combined_size = child.keys.size() + right.keys.size();

        if (combined_size <= kLeafMaxKeys) {
            child.keys.insert(child.keys.end(), right.keys.begin(), right.keys.end());
            child.values.insert(child.values.end(), right.values.begin(), right.values.end());
            child.next_leaf = right.next_leaf;
            SerializeLeaf(child, child_page->Data());
            buffer_pool_->MarkDirty(child_pid);
            parent.keys.erase(parent.keys.begin() + static_cast<long>(idx));
            parent.children.erase(parent.children.begin() + static_cast<long>(idx) + 1);
            return;
        }

        std::vector<int64_t> keys = child.keys;
        keys.insert(keys.end(), right.keys.begin(), right.keys.end());
        std::vector<RID> values = child.values;
        values.insert(values.end(), right.values.begin(), right.values.end());

        // Same relaxation as the left-sibling case above: capacity-only
        // bounds, `ideal` merely a preference.
        size_t min_split = combined_size - kLeafMaxKeys;
        size_t max_split = kLeafMaxKeys;
        size_t ideal = kLeafMinKeys;  // bring child just past the underflow floor, like a minimal borrow
        size_t split_at =
            FindRebalanceSplit(keys, min_split, max_split, ideal).value_or(std::clamp(ideal, min_split, max_split));

        child.keys.assign(keys.begin(), keys.begin() + static_cast<long>(split_at));
        child.values.assign(values.begin(), values.begin() + static_cast<long>(split_at));
        right.keys.assign(keys.begin() + static_cast<long>(split_at), keys.end());
        right.values.assign(values.begin() + static_cast<long>(split_at), values.end());
        parent.keys[idx] = right.keys.front();
        SerializeLeaf(right, right_page->Data());
        SerializeLeaf(child, child_page->Data());
        buffer_pool_->MarkDirty(parent.children[idx + 1]);
        buffer_pool_->MarkDirty(child_pid);
        return;
    }

    // Internal child. Unlike leaves, an internal separator is a routing
    // copy, not data: redistributing one child pointer only needs
    // left's-remainder-max < new-separator <= moved-subtree-min, and that
    // falls straight out of the *existing* parent/left invariants
    // (children[i] holds keys in [keys[i-1], keys[i])) regardless of
    // whether keys repeat elsewhere in the tree -- there's no equivalent
    // of the leaf's "duplicate stranded on the wrong side" failure mode
    // (see docs/DECISIONS.md), so no boundary search or capacity-bounded
    // multi-move is needed here -- the plain single-child-pointer borrow
    // below is always safe on its own.
    InternalNode child = DeserializeInternal(child_page->Data());

    if (has_left) {
        LockPage(object_id_, parent.children[idx - 1], LockMode::kExclusive);
        Page* left_page = buffer_pool_->FetchPage(parent.children[idx - 1]);
        InternalNode left = DeserializeInternal(left_page->Data());
        if (left.keys.size() > kInternalMinKeys) {
            PageId moved_child = left.children.back();
            left.children.pop_back();
            int64_t demoted = parent.keys[idx - 1];
            child.keys.insert(child.keys.begin(), demoted);
            child.children.insert(child.children.begin(), moved_child);
            int64_t promoted = left.keys.back();
            left.keys.pop_back();
            parent.keys[idx - 1] = promoted;
            SerializeInternal(left, left_page->Data());
            SerializeInternal(child, child_page->Data());
            buffer_pool_->MarkDirty(parent.children[idx - 1]);
            buffer_pool_->MarkDirty(child_pid);
            return;
        }
    }
    if (has_right) {
        LockPage(object_id_, parent.children[idx + 1], LockMode::kExclusive);
        Page* right_page = buffer_pool_->FetchPage(parent.children[idx + 1]);
        InternalNode right = DeserializeInternal(right_page->Data());
        if (right.keys.size() > kInternalMinKeys) {
            PageId moved_child = right.children.front();
            right.children.erase(right.children.begin());
            int64_t demoted = parent.keys[idx];
            child.keys.push_back(demoted);
            child.children.push_back(moved_child);
            int64_t promoted = right.keys.front();
            right.keys.erase(right.keys.begin());
            parent.keys[idx] = promoted;
            SerializeInternal(right, right_page->Data());
            SerializeInternal(child, child_page->Data());
            buffer_pool_->MarkDirty(parent.children[idx + 1]);
            buffer_pool_->MarkDirty(child_pid);
            return;
        }
    }

    // Merge, rotating the parent's separator key down into the combined
    // node (this is the one place an internal node's key doesn't come
    // from a split -- it's the old boundary between two subtrees that are
    // now one). Safe for the same reason as the leaf merge above: we only
    // get here when neither sibling had spare capacity to redistribute
    // from, so combined size <= 2*kInternalMinKeys < kInternalMaxKeys.
    if (has_left) {
        LockPage(object_id_, parent.children[idx - 1], LockMode::kExclusive);
        Page* left_page = buffer_pool_->FetchPage(parent.children[idx - 1]);
        InternalNode left = DeserializeInternal(left_page->Data());
        left.keys.push_back(parent.keys[idx - 1]);
        left.keys.insert(left.keys.end(), child.keys.begin(), child.keys.end());
        left.children.insert(left.children.end(), child.children.begin(), child.children.end());
        SerializeInternal(left, left_page->Data());
        buffer_pool_->MarkDirty(parent.children[idx - 1]);
        parent.keys.erase(parent.keys.begin() + static_cast<long>(idx) - 1);
        parent.children.erase(parent.children.begin() + static_cast<long>(idx));
    } else {
        LockPage(object_id_, parent.children[idx + 1], LockMode::kExclusive);
        Page* right_page = buffer_pool_->FetchPage(parent.children[idx + 1]);
        InternalNode right = DeserializeInternal(right_page->Data());
        child.keys.push_back(parent.keys[idx]);
        child.keys.insert(child.keys.end(), right.keys.begin(), right.keys.end());
        child.children.insert(child.children.end(), right.children.begin(), right.children.end());
        SerializeInternal(child, child_page->Data());
        buffer_pool_->MarkDirty(child_pid);
        parent.keys.erase(parent.keys.begin() + static_cast<long>(idx));
        parent.children.erase(parent.children.begin() + static_cast<long>(idx) + 1);
    }
}

}  // namespace flintdb
