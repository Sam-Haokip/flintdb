#include "../src/storage/disk_manager.h"
#include "test_framework.h"
#include "test_utils.h"

#include <cstring>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

using namespace flintdb;
using flintdb::testing::TempFile;

FLINTDB_TEST(disk_manager_allocate_grows_file_and_numbers_pages_from_zero) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    FLINTDB_CHECK_EQ(dm.NumPages(), 0u);
    PageId p0 = dm.AllocatePage();
    PageId p1 = dm.AllocatePage();
    FLINTDB_CHECK_EQ(p0, 0u);
    FLINTDB_CHECK_EQ(p1, 1u);
    FLINTDB_CHECK_EQ(dm.NumPages(), 2u);
}

FLINTDB_TEST(disk_manager_write_then_read_roundtrips_exactly) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId p = dm.AllocatePage();

    std::string payload = "flintdb-page-payload";
    char out[PAGE_SIZE];
    std::memset(out, 0, PAGE_SIZE);
    std::memcpy(out, payload.data(), payload.size());
    dm.WritePage(p, out);

    char in[PAGE_SIZE];
    dm.ReadPage(p, in);
    FLINTDB_CHECK(std::memcmp(out, in, PAGE_SIZE) == 0);
}

FLINTDB_TEST(disk_manager_second_page_does_not_overwrite_first) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId p0 = dm.AllocatePage();
    PageId p1 = dm.AllocatePage();

    char buf0[PAGE_SIZE];
    std::memset(buf0, 'A', PAGE_SIZE);
    dm.WritePage(p0, buf0);

    char buf1[PAGE_SIZE];
    std::memset(buf1, 'B', PAGE_SIZE);
    dm.WritePage(p1, buf1);

    char check0[PAGE_SIZE];
    dm.ReadPage(p0, check0);
    FLINTDB_CHECK(std::memcmp(buf0, check0, PAGE_SIZE) == 0);

    char check1[PAGE_SIZE];
    dm.ReadPage(p1, check1);
    FLINTDB_CHECK(std::memcmp(buf1, check1, PAGE_SIZE) == 0);
}

FLINTDB_TEST(disk_manager_reopens_existing_file_and_resumes_page_numbering) {
    TempFile tmp;
    {
        DiskManager dm(tmp.path());
        dm.AllocatePage();
        dm.AllocatePage();
        dm.AllocatePage();
    }
    DiskManager dm2(tmp.path());
    FLINTDB_CHECK_EQ(dm2.NumPages(), 3u);
    PageId p = dm2.AllocatePage();
    FLINTDB_CHECK_EQ(p, 3u);
}

FLINTDB_TEST(disk_manager_reset_file_truncates_and_restarts_numbering) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    dm.AllocatePage();
    dm.AllocatePage();
    FLINTDB_CHECK_EQ(dm.NumPages(), 2u);

    dm.ResetFile();
    FLINTDB_CHECK_EQ(dm.NumPages(), 0u);
    PageId p = dm.AllocatePage();
    FLINTDB_CHECK_EQ(p, 0u);
}

FLINTDB_TEST(disk_manager_read_or_write_beyond_end_of_file_throws) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    dm.AllocatePage();  // page 0 only

    char buf[PAGE_SIZE];
    bool threw_on_read = false;
    try {
        dm.ReadPage(5, buf);
    } catch (const std::out_of_range&) {
        threw_on_read = true;
    }
    FLINTDB_CHECK(threw_on_read);

    bool threw_on_write = false;
    try {
        dm.WritePage(5, buf);
    } catch (const std::out_of_range&) {
        threw_on_write = true;
    }
    FLINTDB_CHECK(threw_on_write);
}

FLINTDB_TEST(disk_manager_rejects_a_file_whose_size_is_not_page_aligned) {
    TempFile tmp;
    {
        // Write a file with a size that is not a multiple of PAGE_SIZE —
        // simulating a corrupted or foreign file — before DiskManager
        // ever opens it.
        std::ofstream f(tmp.path(), std::ios::binary);
        std::string junk(PAGE_SIZE + 17, 'z');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    bool threw = false;
    try {
        DiskManager dm(tmp.path());
        (void)dm;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    FLINTDB_CHECK(threw);
}

FLINTDB_TEST(disk_manager_concurrent_allocate_never_hands_out_a_duplicate_page_id) {
    // Phase 4: many threads racing AllocatePage on the same DiskManager
    // must each get a distinct id covering exactly [0, N), with the file
    // ending up exactly N pages long -- proving next_page_id_'s
    // increment-then-ftruncate is genuinely atomic under real
    // concurrency, not just single-threaded-correct.
    TempFile tmp;
    DiskManager dm(tmp.path());

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::vector<PageId>> results(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&dm, &results, t] {
            results[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                results[t].push_back(dm.AllocatePage());
            }
        });
    }
    for (auto& th : threads) th.join();

    std::set<PageId> all_ids;
    for (auto& per_thread : results) {
        for (PageId id : per_thread) all_ids.insert(id);
    }
    FLINTDB_CHECK_EQ(all_ids.size(), static_cast<size_t>(kThreads * kPerThread));
    FLINTDB_CHECK_EQ(*all_ids.begin(), 0u);
    FLINTDB_CHECK_EQ(*all_ids.rbegin(), static_cast<PageId>(kThreads * kPerThread - 1));
    FLINTDB_CHECK_EQ(dm.NumPages(), static_cast<size_t>(kThreads * kPerThread));
}

FLINTDB_TEST(disk_manager_concurrent_reads_and_writes_to_different_pages_do_not_corrupt_each_other) {
    // Each thread owns exactly one pre-allocated page and repeatedly
    // writes-then-reads-back a byte pattern unique to that page, all
    // threads running at once -- proving concurrent pread/pwrite at
    // different offsets on the same fd never bleed into each other, and
    // that ReadPage/WritePage's bounds check races correctly with
    // AllocatePage having already grown the file for every page used
    // here.
    TempFile tmp;
    DiskManager dm(tmp.path());

    constexpr int kThreads = 8;
    std::vector<PageId> pages;
    for (int t = 0; t < kThreads; ++t) pages.push_back(dm.AllocatePage());

    std::vector<std::thread> threads;
    // Deliberately std::vector<char>, not std::vector<bool>: vector<bool>
    // is bit-packed, so distinct elements can share an underlying storage
    // word -- writing ok[t] from different threads for different t would
    // then race on that shared word even though the indices themselves
    // don't overlap. This is a real trap ThreadSanitizer caught here
    // during Phase 4 (see docs/DECISIONS.md): a genuine data race, but in
    // this test's own bookkeeping, not in DiskManager. vector<char> gives
    // every element its own byte, so no such aliasing is possible.
    std::vector<char> ok(kThreads, 0);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&dm, &pages, &ok, t] {
            char pattern = static_cast<char>('A' + t);
            char write_buf[PAGE_SIZE];
            std::memset(write_buf, pattern, PAGE_SIZE);

            bool all_rounds_ok = true;
            for (int round = 0; round < 50; ++round) {
                dm.WritePage(pages[static_cast<size_t>(t)], write_buf);
                char read_buf[PAGE_SIZE];
                dm.ReadPage(pages[static_cast<size_t>(t)], read_buf);
                if (std::memcmp(write_buf, read_buf, PAGE_SIZE) != 0) all_rounds_ok = false;
            }
            ok[static_cast<size_t>(t)] = all_rounds_ok ? 1 : 0;
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        FLINTDB_CHECK(ok[static_cast<size_t>(t)] == 1);
    }
}
