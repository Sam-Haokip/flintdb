#include "../src/index/btree.h"
#include "../src/storage/disk_manager.h"
#include "test_framework.h"
#include "test_utils.h"

#include <algorithm>
#include <random>
#include <tuple>
#include <unordered_set>

using namespace flintdb;
using flintdb::testing::TempFile;

namespace {
RID MakeRid(uint32_t page_id, uint16_t slot_id) { return RID{page_id, slot_id}; }

bool SameEntries(std::vector<std::pair<int64_t, RID>> a, std::vector<std::pair<int64_t, RID>> b) {
    auto key = [](const std::pair<int64_t, RID>& e) {
        return std::tie(e.first, e.second.page_id, e.second.slot_id);
    };
    std::sort(a.begin(), a.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
    std::sort(b.begin(), b.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
    return a == b;
}
}  // namespace

FLINTDB_TEST(btree_empty_tree_has_no_height_and_finds_nothing) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    FLINTDB_CHECK(tree.Empty());
    FLINTDB_CHECK_EQ(tree.Height(), 0u);
    FLINTDB_CHECK(tree.Search(42).empty());
}

FLINTDB_TEST(btree_single_insert_then_search_finds_it) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    tree.Insert(10, MakeRid(0, 0));
    FLINTDB_CHECK(!tree.Empty());
    FLINTDB_CHECK_EQ(tree.Height(), 1u);

    auto results = tree.Search(10);
    FLINTDB_CHECK_EQ(results.size(), 1u);
    FLINTDB_CHECK(results[0] == MakeRid(0, 0));
    FLINTDB_CHECK(tree.Search(99).empty());
}

FLINTDB_TEST(btree_search_supports_duplicate_keys) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    tree.Insert(5, MakeRid(0, 0));
    tree.Insert(5, MakeRid(0, 1));
    tree.Insert(5, MakeRid(1, 0));

    auto results = tree.Search(5);
    FLINTDB_CHECK_EQ(results.size(), 3u);
    std::unordered_set<uint64_t> seen;
    for (auto& r : results) seen.insert((static_cast<uint64_t>(r.page_id) << 16) | r.slot_id);
    FLINTDB_CHECK_EQ(seen.size(), 3u);
}

FLINTDB_TEST(btree_many_sequential_inserts_force_leaf_splits_and_all_remain_findable) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    const int64_t n = 2000;  // comfortably more than one leaf (~291 keys) can hold
    for (int64_t k = 0; k < n; k++) {
        tree.Insert(k, MakeRid(static_cast<uint32_t>(k), 0));
    }

    FLINTDB_CHECK(tree.Height() > 1);  // must have grown past a single leaf

    for (int64_t k = 0; k < n; k++) {
        auto results = tree.Search(k);
        FLINTDB_CHECK_EQ(results.size(), 1u);
        FLINTDB_CHECK(results[0] == MakeRid(static_cast<uint32_t>(k), 0));
    }
    FLINTDB_CHECK(tree.Search(n).empty());       // just past the end
    FLINTDB_CHECK(tree.Search(-1).empty());      // just before the start

    auto all = tree.RangeScanAll();
    FLINTDB_CHECK_EQ(all.size(), static_cast<size_t>(n));
    for (int64_t k = 0; k < n; k++) {
        FLINTDB_CHECK_EQ(all[static_cast<size_t>(k)].first, k);  // ascending order
    }
}

FLINTDB_TEST(btree_random_order_inserts_also_all_remain_findable) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    const int64_t n = 3000;
    std::vector<int64_t> keys(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; i++) keys[static_cast<size_t>(i)] = i;
    std::mt19937 gen(1234);
    std::shuffle(keys.begin(), keys.end(), gen);

    for (int64_t k : keys) tree.Insert(k, MakeRid(static_cast<uint32_t>(k), 0));

    auto all = tree.RangeScanAll();
    FLINTDB_CHECK_EQ(all.size(), static_cast<size_t>(n));
    for (int64_t i = 0; i < n; i++) {
        FLINTDB_CHECK_EQ(all[static_cast<size_t>(i)].first, i);  // still sorted regardless of insert order
    }
}

FLINTDB_TEST(btree_delete_removes_exactly_the_matching_entry) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    tree.Insert(1, MakeRid(0, 0));
    tree.Insert(2, MakeRid(0, 1));
    tree.Insert(2, MakeRid(0, 2));  // duplicate key, different RID
    tree.Insert(3, MakeRid(0, 3));

    FLINTDB_CHECK(tree.Delete(2, MakeRid(0, 1)));
    auto remaining = tree.Search(2);
    FLINTDB_CHECK_EQ(remaining.size(), 1u);
    FLINTDB_CHECK(remaining[0] == MakeRid(0, 2));

    // Deleting an entry that was never there (wrong RID, and wrong key).
    FLINTDB_CHECK(!tree.Delete(2, MakeRid(9, 9)));
    FLINTDB_CHECK(!tree.Delete(999, MakeRid(0, 0)));
}

FLINTDB_TEST(btree_delete_everything_leaves_an_empty_tree) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    for (int64_t k = 0; k < 50; k++) tree.Insert(k, MakeRid(static_cast<uint32_t>(k), 0));
    for (int64_t k = 0; k < 50; k++) {
        FLINTDB_CHECK(tree.Delete(k, MakeRid(static_cast<uint32_t>(k), 0)));
    }
    FLINTDB_CHECK(tree.Empty());
    FLINTDB_CHECK_EQ(tree.Height(), 0u);
    FLINTDB_CHECK(tree.RangeScanAll().empty());
}

FLINTDB_TEST(btree_insert_and_delete_enough_to_force_merges_across_multiple_levels) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    const int64_t n = 4000;  // enough for several leaf levels' worth of splitting
    for (int64_t k = 0; k < n; k++) tree.Insert(k, MakeRid(static_cast<uint32_t>(k), 0));
    FLINTDB_CHECK(tree.Height() > 1);

    // Delete every key whose value mod 3 == 0 -- enough deletions,
    // scattered enough, to force real merging/redistribution rather than
    // just emptying whole leaves in order.
    for (int64_t k = 0; k < n; k += 3) {
        FLINTDB_CHECK(tree.Delete(k, MakeRid(static_cast<uint32_t>(k), 0)));
    }

    auto all = tree.RangeScanAll();
    size_t expected_count = 0;
    for (int64_t k = 0; k < n; k++) {
        if (k % 3 != 0) expected_count++;
    }
    FLINTDB_CHECK_EQ(all.size(), expected_count);

    for (int64_t k = 0; k < n; k++) {
        auto results = tree.Search(k);
        if (k % 3 == 0) {
            FLINTDB_CHECK(results.empty());
        } else {
            FLINTDB_CHECK_EQ(results.size(), 1u);
        }
    }
}

// The highest-value test in this file: cross-check the tree's full
// contents against a plain in-memory reference model after *every*
// operation, across thousands of randomized inserts and deletes. This is
// the best chance of catching a subtle split/merge/redistribute bug --
// the kind that a handful of hand-picked scenarios above could easily
// miss. Fixed seed so any failure is exactly reproducible, not flaky.
namespace {

// Runs the randomized model-based cross-check for one seed. Pulled out of
// the FLINTDB_TEST body so btree_randomized_model_based_stress_test below
// can drive it across several seeds instead of just one: a single fixed
// seed (42, 4000 ops) is exactly what let the duplicate-key split/
// redistribute/merge bugs documented in docs/DECISIONS.md ship in the
// first place -- it happened not to exercise the failing cases, and
// looked like a clean pass. Multiple seeds, each run long enough to force
// many splits/merges/redistributions of a small (0..500) key range with
// heavy duplication, catch what one lucky seed can miss.
void RunRandomizedStressForSeed(unsigned seed, int kOps) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    BufferPool bp(&dm);
    BPlusTree tree(&bp);

    std::vector<std::pair<int64_t, RID>> model;  // mirrors tree contents exactly
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int64_t> key_dist(0, 500);  // small range: guarantees lots of duplicate keys
    std::uniform_int_distribution<int> op_dist(0, 99);        // 0..59 insert, 60..99 delete
    uint32_t next_page = 0;

    for (int i = 0; i < kOps; i++) {
        int op = op_dist(gen);
        if (op < 60 || model.empty()) {
            int64_t key = key_dist(gen);
            RID rid = MakeRid(next_page++, static_cast<uint16_t>(i % 65536));
            tree.Insert(key, rid);
            model.emplace_back(key, rid);
        } else {
            std::uniform_int_distribution<size_t> pick(0, model.size() - 1);
            size_t idx = pick(gen);
            auto [key, rid] = model[idx];
            bool deleted = tree.Delete(key, rid);
            FLINTDB_CHECK(deleted);
            model.erase(model.begin() + static_cast<long>(idx));
        }

        // Cross-check full contents, and the tree's own structural
        // invariants, every 25 ops (every op would make this test slow
        // without adding much more confidence). ValidateInvariants catches
        // a routing violation (e.g. a duplicate stranded across a leaf
        // boundary) directly, without waiting for a later Search/Delete to
        // happen to probe the exact stranded key -- see its comment.
        if (i % 25 == 0 || i == kOps - 1) {
            auto tree_contents = tree.RangeScanAll();
            FLINTDB_CHECK_EQ(tree_contents.size(), model.size());
            FLINTDB_CHECK(SameEntries(tree_contents, model));
            auto invariant_error = tree.ValidateInvariants();
            if (invariant_error.has_value()) {
                flintdb::testing::Fail("btree invariant violation (seed " + std::to_string(seed) +
                                            ", op " + std::to_string(i) + "): " + *invariant_error,
                                        __FILE__, __LINE__);
            }
        }
    }

    // Final check regardless of the sampling above.
    FLINTDB_CHECK(SameEntries(tree.RangeScanAll(), model));

    // Spot-check Search() too, not just RangeScanAll().
    std::unordered_set<int64_t> distinct_keys;
    for (auto& [k, r] : model) distinct_keys.insert(k);
    for (int64_t k : distinct_keys) {
        std::vector<RID> expected;
        for (auto& [mk, mr] : model) {
            if (mk == k) expected.push_back(mr);
        }
        auto actual = tree.Search(k);
        FLINTDB_CHECK_EQ(actual.size(), expected.size());
    }
}

}  // namespace

FLINTDB_TEST(btree_randomized_model_based_stress_test) {
    // Ten different seeds, each long enough to force many splits, merges,
    // and redistributions -- see RunRandomizedStressForSeed's comment for
    // why more than one fixed seed matters here.
    for (unsigned seed = 1; seed <= 10; seed++) {
        RunRandomizedStressForSeed(seed, 4000);
    }
}
