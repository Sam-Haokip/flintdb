// Phase 2 benchmark: sequential scan vs. B+-tree index lookup, on a table
// of N rows keyed by a plain integer. Builds the structures once, then
// times many individual point lookups so scripts/bench_index_vs_scan.py
// can compute mean/stdev across a real sample, not a single run.
//
// Usage: bench_index_vs_scan <N> <num_lookups> [seed]
// Output: CSV to stdout, one row per (method, lookup) sample.

#include "../src/index/btree.h"
#include "../src/storage/heap_file.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>

using namespace flintdb;

namespace {

std::string MakeRow(int64_t key) {
    // Fixed-format row: "key=<k>,payload=<20 x's>" -- realistic enough
    // (a real row with an indexed integer column plus other data) while
    // staying trivial to search for without a schema layer (Phase 5).
    return "key=" + std::to_string(key) + ",payload=xxxxxxxxxxxxxxxxxxxx";
}

bool RowHasKey(const std::string& row, int64_t key) {
    std::string prefix = "key=" + std::to_string(key) + ",";
    return row.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <N> <num_lookups> [seed]\n";
        return 2;
    }
    size_t n = static_cast<size_t>(std::stoul(argv[1]));
    size_t num_lookups = static_cast<size_t>(std::stoul(argv[2]));
    unsigned seed = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 42u;

    std::string heap_path = "/tmp/flintdb_bench_heap_" + std::to_string(getpid()) + ".db";
    std::string index_path = "/tmp/flintdb_bench_index_" + std::to_string(getpid()) + ".db";
    std::remove(heap_path.c_str());
    std::remove(index_path.c_str());

    {
        DiskManager heap_dm(heap_path);
        BufferPool heap_bp(&heap_dm);
        HeapFile heap(0, &heap_bp);

        DiskManager index_dm(index_path);
        BufferPool index_bp(&index_dm);
        BPlusTree tree(0, &index_bp);

        std::mt19937 gen(seed);
        std::vector<int64_t> keys(n);
        for (size_t i = 0; i < n; i++) keys[i] = static_cast<int64_t>(i);
        std::shuffle(keys.begin(), keys.end(), gen);  // insert in random order: realistic, not best-case-sorted

        for (size_t i = 0; i < n; i++) {
            RID rid = heap.Insert(MakeRow(keys[i]));
            tree.Insert(keys[i], rid);
        }

        std::uniform_int_distribution<size_t> pick(0, n == 0 ? 0 : n - 1);
        std::vector<int64_t> lookup_keys(num_lookups);
        for (size_t i = 0; i < num_lookups; i++) lookup_keys[i] = keys[pick(gen)];

        std::cout << "method,n,trial,microseconds\n";

        for (size_t t = 0; t < num_lookups; t++) {
            int64_t target = lookup_keys[t];
            auto start = std::chrono::high_resolution_clock::now();
            auto found = heap.Find([target](const std::string& row) { return RowHasKey(row, target); });
            auto end = std::chrono::high_resolution_clock::now();
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            if (!found.has_value()) {
                std::cerr << "bench error: scan did not find key " << target << " that was just inserted\n";
                return 1;
            }
            std::cout << "scan," << n << "," << t << "," << micros << "\n";
        }

        for (size_t t = 0; t < num_lookups; t++) {
            int64_t target = lookup_keys[t];
            auto start = std::chrono::high_resolution_clock::now();
            auto results = tree.Search(target);
            auto end = std::chrono::high_resolution_clock::now();
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            if (results.empty()) {
                std::cerr << "bench error: index did not find key " << target << " that was just inserted\n";
                return 1;
            }
            std::cout << "index," << n << "," << t << "," << micros << "\n";
        }
    }

    std::remove(heap_path.c_str());
    std::remove(index_path.c_str());
    return 0;
}
