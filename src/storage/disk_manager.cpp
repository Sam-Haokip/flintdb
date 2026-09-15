#include "disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace flintdb {

namespace {

// pread/pwrite are permitted by POSIX to transfer fewer bytes than
// requested (short reads/writes), so a correct page I/O routine has to
// loop rather than trust a single call. These two helpers are the only
// place that has to know that.

void FullPRead(int fd, void* buf, size_t count, off_t offset) {
    char* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t n = pread(fd, p + done, count - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("DiskManager: pread failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            throw std::runtime_error("DiskManager: pread hit EOF before a full page was read");
        }
        done += static_cast<size_t>(n);
    }
}

void FullPWrite(int fd, const void* buf, size_t count, off_t offset) {
    const char* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t n = pwrite(fd, p + done, count - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("DiskManager: pwrite failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
}

}  // namespace

DiskManager::DiskManager(const std::string& db_file_path) {
    fd_ = open(db_file_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("DiskManager: failed to open " + db_file_path + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        int saved_errno = errno;
        close(fd_);
        throw std::runtime_error(std::string("DiskManager: fstat failed: ") + std::strerror(saved_errno));
    }
    if (st.st_size % static_cast<off_t>(PAGE_SIZE) != 0) {
        close(fd_);
        throw std::runtime_error("DiskManager: " + db_file_path +
                                  " size is not a multiple of PAGE_SIZE (corrupted or foreign file)");
    }
    next_page_id_ = static_cast<PageId>(st.st_size / static_cast<off_t>(PAGE_SIZE));
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) close(fd_);
}

PageId DiskManager::AllocatePage() {
    PageId id = next_page_id_++;
    off_t new_size = static_cast<off_t>(id + 1) * static_cast<off_t>(PAGE_SIZE);
    if (ftruncate(fd_, new_size) != 0) {
        throw std::runtime_error(std::string("DiskManager: ftruncate failed: ") + std::strerror(errno));
    }
    return id;
}

void DiskManager::ReadPage(PageId page_id, char* out_buf) const {
    if (page_id >= next_page_id_) {
        throw std::out_of_range("DiskManager::ReadPage: page_id beyond end of file");
    }
    FullPRead(fd_, out_buf, PAGE_SIZE, static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE));
}

void DiskManager::WritePage(PageId page_id, const char* buf) {
    if (page_id >= next_page_id_) {
        throw std::out_of_range("DiskManager::WritePage: page_id beyond end of file");
    }
    FullPWrite(fd_, buf, PAGE_SIZE, static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE));
}

size_t DiskManager::NumPages() const { return next_page_id_; }

void DiskManager::ResetFile() {
    if (ftruncate(fd_, 0) != 0) {
        throw std::runtime_error(std::string("DiskManager::ResetFile: ftruncate failed: ") + std::strerror(errno));
    }
    next_page_id_ = 0;
}

}  // namespace flintdb
