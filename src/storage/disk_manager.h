#pragma once
#include "../common/config.h"

#include <mutex>
#include <string>

namespace flintdb {

// Owns a single fixed-page-size file on disk: allocating new pages and
// reading/writing whole pages at their page-number offset (page_id *
// PAGE_SIZE). Holds exactly one open file descriptor for its lifetime.
//
// Thread-safe as of Phase 4: `next_page_id_` (the only shared mutable
// state) is guarded by `mutex_`, held across each of AllocatePage's and
// ResetFile's full body -- including the ftruncate call -- so a
// concurrent ReadPage/WritePage bounds check can never observe a
// next_page_id_ that has been bumped without the corresponding
// ftruncate having already completed. The actual page I/O
// (FullPRead/FullPWrite) deliberately happens *outside* the lock: it
// uses pread/pwrite with an explicit offset rather than the shared
// file-position read/write, which POSIX guarantees is safe to call
// concurrently on the same fd at different offsets, so serializing it
// through this mutex would only add contention with no correctness
// benefit -- and would undermine the whole point of Phase 4's
// page-level locking, which is that two transactions touching
// different pages should genuinely run concurrently, down to the disk
// I/O itself. This is a different, lower-level kind of concurrency
// control than LockManager's page locks: it protects DiskManager's own
// bookkeeping, not transaction isolation.
//
// Not crash-safe either: writes here go straight to the data file with
// no logging in front of them, which is exactly what Phase 3's WAL
// exists to fix. Phase 1 makes no durability claim (see docs/SPEC.md).
class DiskManager {
 public:
    explicit DiskManager(const std::string& db_file_path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Grows the file by one page and returns the new page's id. Page ids
    // are handed out monotonically starting at 0 and are never reused
    // within a DiskManager's lifetime (no free-list in Phase 1).
    PageId AllocatePage();

    // Reads/writes exactly PAGE_SIZE bytes at page_id's offset. Throws
    // std::out_of_range if page_id was never allocated.
    void ReadPage(PageId page_id, char* out_buf) const;
    void WritePage(PageId page_id, const char* buf);

    size_t NumPages() const;

    // Truncates the file to empty and restarts page numbering at 0. Used
    // by HeapFile's Phase-1 "delete-by-rewrite" strategy — see
    // docs/DECISIONS.md for why deletes work this way in Phase 1.
    void ResetFile();

 private:
    int fd_;
    PageId next_page_id_;
    mutable std::mutex mutex_;
};

}  // namespace flintdb
