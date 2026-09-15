#pragma once
#include <filesystem>
#include <string>

namespace flintdb::testing {

// A unique on-disk file path that deletes itself on destruction, so each
// test gets a clean, isolated .db file and doesn't leak temp files.
class TempFile {
 public:
    TempFile() {
        auto dir = std::filesystem::temp_directory_path();
        path_ = (dir / ("flintdb_test_" + std::to_string(counter_++) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
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

}  // namespace flintdb::testing
