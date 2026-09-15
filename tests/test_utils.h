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

}  // namespace flintdb::testing
