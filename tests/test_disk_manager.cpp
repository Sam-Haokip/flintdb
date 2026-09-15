#include "../src/storage/disk_manager.h"
#include "test_framework.h"
#include "test_utils.h"

#include <cstring>
#include <fstream>

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
