#pragma once
#include "../common/config.h"
#include "log_record.h"

#include <string>
#include <vector>

namespace flintdb {

// Owns a single write-ahead-log file: appending Begin/Update/Commit/Abort
// records, fsync'ing them durable on Flush(), and -- on construction --
// scanning whatever is already on disk (from a prior run) into memory,
// discarding a torn tail left by a crash mid-write to the log itself.
//
// This is the actual durability mechanism for the whole engine (see
// docs/SPEC.md section 2 and section 5's verification standard): nothing
// else in FlintDB calls fsync. A record is only durable -- guaranteed to
// still be there after a crash -- once Flush() has returned; appending a
// record only guarantees it's visible to *this* LogManager (and to a
// reopen in the same process/OS session), not that it survived a crash.
//
// On-disk record format (all integers native-endian, matching the rest of
// the codebase's memcpy-based page layout -- see page.cpp -- rather than
// a portable wire format, since a single FlintDB file is never read on a
// different machine):
//
//   [ lsn:8 | type:1 | txn_id:8 | page_id:4 | payload_len:4 | payload... | checksum:4 ]
//
// payload_len is 0 for kBegin/kCommit/kAbort and PAGE_SIZE for kUpdate
// (whose payload is the full new page image). checksum is an FNV-1a hash
// over every byte from lsn through the end of payload -- it's what lets
// LogManager tell a genuine record apart from a torn or corrupted one on
// reopen (see the constructor's doc comment).
//
// Not thread-safe -- concurrent access is a Phase 4 concern layered above
// this, same as DiskManager and BufferPool.
class LogManager {
 public:
    // Opens (or creates) `wal_file_path` and immediately scans it: every
    // record is re-checksummed, and the first record that fails to parse
    // (not enough bytes left for its header, an unrecognized type, a
    // payload_len that doesn't match its type, or a checksum mismatch) is
    // treated as a torn write -- the write that was physically in
    // progress when a crash happened -- rather than as an error. The file
    // is truncated back to the end of the last good record (this is the
    // "truncation-on-open" that makes the WAL file itself crash-safe to
    // reopen), and every record before that point is kept, in file order
    // (which is LSN order, since LSNs are assigned by appending), for
    // RecordsOnOpen() to hand to RunRecovery.
    explicit LogManager(const std::string& wal_file_path);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // The valid records found at construction time, in LSN order. Fixed
    // at construction -- later Append* calls do not appear here, and
    // neither does a later Checkpoint() retroactively empty it. RunRecovery
    // (recovery.h) is handed only this vector; it never reads the file.
    const std::vector<LogRecord>& RecordsOnOpen() const;

    // Each Append* call writes one record and returns its assigned LSN.
    // None of them call Flush(): appending makes a record visible to a
    // reopen of this same file right away (it's gone through write()),
    // but not durable against an actual crash until Flush() has run.
    // Callers decide when that matters -- see transaction_manager.h's
    // Commit(), which appends the Commit record and then calls Flush()
    // immediately, since that's the specific point that makes a
    // transaction durably committed.
    Lsn AppendBegin(TxnId txn_id);
    Lsn AppendUpdate(TxnId txn_id, PageId page_id, const char* page_image);
    Lsn AppendCommit(TxnId txn_id);
    Lsn AppendAbort(TxnId txn_id);

    // fsyncs the WAL file. Every record appended so far becomes durable;
    // nothing appended after this call is covered until Flush() is called
    // again.
    void Flush();

    // Wipes the WAL file back to empty and resets the LSN counter to 1.
    // Only correct to call when every already-appended record's effects
    // are already durable in the data file (i.e. no transaction is
    // active and BufferPool::FlushAll() has just been called) -- see
    // docs/DECISIONS.md D-018 for why this "quiescent checkpoint" is a
    // deliberate simplification over ARIES-style fuzzy checkpointing
    // (which would keep taking writes during the checkpoint), and what
    // would have to change to lift that restriction.
    void Checkpoint();

    // The LSN the *next* Append* call will assign. Exposed for tests and
    // for callers that want to reason about ordering without appending a
    // probe record.
    Lsn NextLsn() const;

 private:
    Lsn AppendRecord(LogRecordType type, TxnId txn_id, PageId page_id, const char* payload,
                      uint32_t payload_len);

    int fd_;
    Lsn next_lsn_;
    std::vector<LogRecord> records_on_open_;
};

}  // namespace flintdb
