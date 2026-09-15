#include "log_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace flintdb {

namespace {

// Header layout, matching log_manager.h's doc comment:
//   lsn:8  type:1  txn_id:8  page_id:4  payload_len:4
constexpr size_t kOffLsn = 0;
constexpr size_t kOffType = 8;
constexpr size_t kOffTxnId = 9;
constexpr size_t kOffPageId = 17;
constexpr size_t kOffPayloadLen = 21;
constexpr size_t kHeaderSize = 25;
constexpr size_t kChecksumSize = 4;

template <typename T>
T ReadAt(const char* buf, size_t offset) {
    T v;
    std::memcpy(&v, buf + offset, sizeof(T));
    return v;
}

template <typename T>
void WriteAt(char* buf, size_t offset, T v) {
    std::memcpy(buf + offset, &v, sizeof(T));
}

// FNV-1a, 32-bit. Not cryptographic -- it only needs to catch torn writes
// and random bit-rot, not a malicious adversary, which is exactly the
// threat model the rest of the engine assumes (see disk_manager.h). Kept
// in-house rather than pulled in from a library, consistent with the
// project's "own everything" approach to its core data-integrity code
// (docs/DECISIONS.md D-009).
uint32_t Fnv1a32(const char* data, size_t len) {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 0x01000193u;
    }
    return hash;
}

bool IsValidType(uint8_t raw_type) {
    return raw_type == static_cast<uint8_t>(LogRecordType::kBegin) ||
           raw_type == static_cast<uint8_t>(LogRecordType::kUpdate) ||
           raw_type == static_cast<uint8_t>(LogRecordType::kCommit) ||
           raw_type == static_cast<uint8_t>(LogRecordType::kAbort);
}

// The only two payload lengths any genuine record can have. Anything else
// found on disk is corruption (or a torn write), never a valid record.
uint32_t ExpectedPayloadLen(LogRecordType type) {
    return type == LogRecordType::kUpdate ? static_cast<uint32_t>(PAGE_SIZE) : 0u;
}

void FullWrite(int fd, const void* buf, size_t count) {
    const char* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("LogManager: write failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
}

// Reads the whole file from its current position (the constructor calls
// this before any Append*, so that's offset 0) into `out`. Ordinary
// read(), not pread() -- fine here since nothing else touches this fd's
// read position, and every later Append* uses write() on an O_APPEND fd,
// which ignores the read position entirely.
std::vector<char> ReadWholeFile(int fd, size_t size) {
    std::vector<char> buf(size);
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, buf.data() + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("LogManager: read failed: ") + std::strerror(errno));
        }
        if (n == 0) break;  // shouldn't happen given fstat's size, but don't spin if it does
        done += static_cast<size_t>(n);
    }
    buf.resize(done);
    return buf;
}

}  // namespace

LogManager::LogManager(const std::string& wal_file_path) : fd_(-1), next_lsn_(1) {
    // O_APPEND makes every later write() land at the true end of file
    // regardless of the fd's read position, which is what lets the
    // constructor read from the front (to parse existing records) and
    // Append* write at the back without either one having to track a
    // manual offset.
    fd_ = open(wal_file_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("LogManager: failed to open " + wal_file_path + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        int saved_errno = errno;
        close(fd_);
        throw std::runtime_error(std::string("LogManager: fstat failed: ") + std::strerror(saved_errno));
    }
    size_t file_size = static_cast<size_t>(st.st_size);
    std::vector<char> data = ReadWholeFile(fd_, file_size);

    // Walk the file record by record. `good_length` is how much of the
    // file parses cleanly; the first record that doesn't is either a torn
    // write (a crash happened while it was being written) or corruption,
    // and either way the right move is to stop there and forget it, not
    // to try to interpret it further.
    size_t offset = 0;
    while (offset + kHeaderSize <= data.size()) {
        Lsn lsn = ReadAt<Lsn>(data.data(), offset + kOffLsn);
        uint8_t raw_type = ReadAt<uint8_t>(data.data(), offset + kOffType);
        TxnId txn_id = ReadAt<TxnId>(data.data(), offset + kOffTxnId);
        PageId page_id = ReadAt<PageId>(data.data(), offset + kOffPageId);
        uint32_t payload_len = ReadAt<uint32_t>(data.data(), offset + kOffPayloadLen);

        if (!IsValidType(raw_type)) break;
        LogRecordType type = static_cast<LogRecordType>(raw_type);
        if (payload_len != ExpectedPayloadLen(type)) break;

        size_t record_len = kHeaderSize + payload_len + kChecksumSize;
        if (offset + record_len > data.size()) break;  // truncated mid-payload or mid-checksum

        uint32_t stored_checksum = ReadAt<uint32_t>(data.data(), offset + kHeaderSize + payload_len);
        uint32_t actual_checksum = Fnv1a32(data.data() + offset, kHeaderSize + payload_len);
        if (stored_checksum != actual_checksum) break;

        LogRecord record;
        record.lsn = lsn;
        record.type = type;
        record.txn_id = txn_id;
        record.page_id = page_id;
        if (payload_len > 0) {
            std::memcpy(record.page_image.data(), data.data() + offset + kHeaderSize, payload_len);
        }
        records_on_open_.push_back(record);

        offset += record_len;
    }

    if (offset < file_size) {
        // A torn (or corrupt) tail was found -- cut it off so the file on
        // disk matches exactly what RecordsOnOpen() returned, and so the
        // next Append* lands right after the last good record instead of
        // after the garbage.
        if (ftruncate(fd_, static_cast<off_t>(offset)) != 0) {
            int saved_errno = errno;
            close(fd_);
            throw std::runtime_error(std::string("LogManager: ftruncate (torn-tail cleanup) failed: ") +
                                      std::strerror(saved_errno));
        }
    }

    if (!records_on_open_.empty()) {
        next_lsn_ = records_on_open_.back().lsn + 1;
    }
}

LogManager::~LogManager() {
    if (fd_ >= 0) close(fd_);
}

const std::vector<LogRecord>& LogManager::RecordsOnOpen() const { return records_on_open_; }

Lsn LogManager::AppendRecord(LogRecordType type, TxnId txn_id, PageId page_id, const char* payload,
                              uint32_t payload_len) {
    // Held across LSN assignment *and* the physical write() -- see the
    // class comment for why both have to be inside one critical section,
    // not just the counter increment.
    std::lock_guard<std::mutex> lock(mutex_);
    Lsn lsn = next_lsn_++;

    std::vector<char> buf(kHeaderSize + payload_len + kChecksumSize);
    WriteAt<Lsn>(buf.data(), kOffLsn, lsn);
    WriteAt<uint8_t>(buf.data(), kOffType, static_cast<uint8_t>(type));
    WriteAt<TxnId>(buf.data(), kOffTxnId, txn_id);
    WriteAt<PageId>(buf.data(), kOffPageId, page_id);
    WriteAt<uint32_t>(buf.data(), kOffPayloadLen, payload_len);
    if (payload_len > 0) {
        std::memcpy(buf.data() + kHeaderSize, payload, payload_len);
    }
    uint32_t checksum = Fnv1a32(buf.data(), kHeaderSize + payload_len);
    WriteAt<uint32_t>(buf.data(), kHeaderSize + payload_len, checksum);

    FullWrite(fd_, buf.data(), buf.size());
    return lsn;
}

Lsn LogManager::AppendBegin(TxnId txn_id) {
    return AppendRecord(LogRecordType::kBegin, txn_id, INVALID_PAGE_ID, nullptr, 0);
}

Lsn LogManager::AppendUpdate(TxnId txn_id, PageId page_id, const char* page_image) {
    return AppendRecord(LogRecordType::kUpdate, txn_id, page_id, page_image, static_cast<uint32_t>(PAGE_SIZE));
}

Lsn LogManager::AppendCommit(TxnId txn_id) {
    return AppendRecord(LogRecordType::kCommit, txn_id, INVALID_PAGE_ID, nullptr, 0);
}

Lsn LogManager::AppendAbort(TxnId txn_id) {
    return AppendRecord(LogRecordType::kAbort, txn_id, INVALID_PAGE_ID, nullptr, 0);
}

void LogManager::Flush() {
    if (fsync(fd_) != 0) {
        throw std::runtime_error(std::string("LogManager::Flush: fsync failed: ") + std::strerror(errno));
    }
}

void LogManager::Checkpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ftruncate(fd_, 0) != 0) {
        throw std::runtime_error(std::string("LogManager::Checkpoint: ftruncate failed: ") + std::strerror(errno));
    }
    next_lsn_ = 1;
}

Lsn LogManager::NextLsn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_lsn_;
}

}  // namespace flintdb
