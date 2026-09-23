#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace flintdb::testing {

// A unique on-disk file path that deletes itself on destruction, so each
// test gets a clean, isolated file and doesn't leak temp files. Defaults
// to a ".db"-style path; pass an explicit extension (e.g. "wal") for a
// test that needs its own independent WAL file alongside a separate
// TempFile for the data file -- LogManager and DiskManager each just take
// whatever path they're given, so the two don't need to share a stem.
class TempFile {
 public:
    explicit TempFile(const std::string& extension = "db") {
        auto dir = std::filesystem::temp_directory_path();
        path_ = (dir / ("flintdb_test_" + std::to_string(counter_++) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)) + "." + extension))
                    .string();
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }

 private:
    std::string path_;
    static inline int counter_ = 0;
};

// A unique on-disk *directory* path that recursively deletes itself (and
// everything a test put in it) on destruction -- the directory-shaped
// counterpart to TempFile, for tests of anything that owns a whole
// directory of files rather than one file (Catalog's metadata file plus
// its per-object data files; Database, once it exists). Deliberately does
// NOT create the directory itself: callers that need "a fresh directory
// that already exists" call std::filesystem::create_directory(path())
// themselves, since some tests (e.g. Catalog's own constructor) want to
// exercise creating it for the first time.
class TempDir {
 public:
    TempDir() {
        auto dir = std::filesystem::temp_directory_path();
        path_ = (dir / ("flintdb_test_dir_" + std::to_string(counter_++) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this))))
                    .string();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }

 private:
    std::string path_;
    static inline int counter_ = 0;
};

// Raw filesystem helpers for simulating a crash directly on an on-disk
// file, deliberately going around whatever object (LogManager,
// DiskManager, ...) normally owns it. This is the technique
// docs/SPEC.md section 5 calls for: perform real operations against a
// real file, then manually truncate or corrupt its raw bytes to imitate
// exactly what a crash mid-write leaves behind, then reopen fresh objects
// over the result and check they cope correctly.

inline size_t FileSizeOf(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("FileSizeOf: open failed");
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    return static_cast<size_t>(size);
}

// Cuts the file off at `new_size` bytes -- simulates a crash that
// happened partway through a write, leaving everything after `new_size`
// as if it had never been written.
inline void TruncateFileTo(const std::string& path, size_t new_size) {
    if (truncate(path.c_str(), static_cast<off_t>(new_size)) != 0) {
        throw std::runtime_error("TruncateFileTo: truncate failed");
    }
}

// Flips one byte at `offset` -- simulates bit-rot/corruption landing
// inside an already-written region, as opposed to TruncateFileTo's
// "never finished writing" simulation.
inline void CorruptByteAt(const std::string& path, size_t offset) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("CorruptByteAt: open failed");
    char byte;
    if (pread(fd, &byte, 1, static_cast<off_t>(offset)) != 1) {
        close(fd);
        throw std::runtime_error("CorruptByteAt: pread failed");
    }
    byte = static_cast<char>(byte ^ 0xFF);
    if (pwrite(fd, &byte, 1, static_cast<off_t>(offset)) != 1) {
        close(fd);
        throw std::runtime_error("CorruptByteAt: pwrite failed");
    }
    close(fd);
}

// Overwrites the raw bytes of `value` at `offset` -- unlike
// CorruptByteAt's single-bit-flip (good for "some byte got corrupted, we
// don't care to what"), this sets an exact value, for a test that needs
// to engineer a *specific* corrupted value -- e.g. a storage-corruption
// test (docs/DECISIONS.md D-054) that makes a B+-tree internal node's
// child pointer point back at its own page, deliberately forming a
// cycle no bit-flip could reliably be relied on to produce.
template <typename T>
void WriteRawValueAt(const std::string& path, size_t offset, T value) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("WriteRawValueAt: open failed");
    if (pwrite(fd, &value, sizeof(T), static_cast<off_t>(offset)) != static_cast<ssize_t>(sizeof(T))) {
        close(fd);
        throw std::runtime_error("WriteRawValueAt: pwrite failed");
    }
    close(fd);
}

// Reads sizeof(T) raw bytes at `offset` back out as a T -- the read-side
// counterpart to WriteRawValueAt, for a test that needs to inspect an
// on-disk value (e.g. a page's own PageId/PageType tag) before deciding
// what to corrupt and how.
template <typename T>
T ReadRawValueAt(const std::string& path, size_t offset) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("ReadRawValueAt: open failed");
    T value;
    if (pread(fd, &value, sizeof(T), static_cast<off_t>(offset)) != static_cast<ssize_t>(sizeof(T))) {
        close(fd);
        throw std::runtime_error("ReadRawValueAt: pread failed");
    }
    close(fd);
    return value;
}

}  // namespace flintdb::testing
