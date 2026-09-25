#pragma once
#include "../common/config.h"
#include "log_record.h"

#include <condition_variable>
#include <mutex>
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
//   [ lsn:8 | type:1 | txn_id:8 | object_id:4 | page_id:4 | payload_len:4 | payload... | checksum:4 ]
//
// object_id (Phase 5, docs/DECISIONS.md D-032) identifies which table/index's
// file page_id refers to -- see log_record.h's LogRecord::object_id for why
// a bare page_id stopped being globally unique once every table/index got
// its own file (docs/DECISIONS.md D-031).
//
// payload_len is 0 for kBegin/kCommit/kAbort and PAGE_SIZE for kUpdate
// (whose payload is the full new page image). checksum is an FNV-1a hash
// over every byte from lsn through the end of payload -- it's what lets
// LogManager tell a genuine record apart from a torn or corrupted one on
// reopen (see the constructor's doc comment).
//
// Thread-safe as of Phase 4: mutex_ guards next_lsn_ and serializes each
// AppendRecord call's full body -- LSN assignment *and* the physical
// write() -- as one atomic unit. That's the specific property real
// concurrency needs here: without it, two threads could interleave so
// that the record with the *later* LSN physically lands in the file
// before the one with the *earlier* LSN (each individual write() is
// atomic thanks to O_APPEND, but nothing otherwise stops two threads'
// write() calls from completing in the opposite order to their LSN
// assignment). RunRecovery relies on file order matching LSN order
// (see wal/recovery.h and the class comment above), so that guarantee
// has to be held from the moment a record's LSN is handed out through
// the moment its bytes are durably ordered in the file, not just
// protect the counter increment in isolation.
//
// Flush() (fsync) and RecordsOnOpen() (fixed at construction, never
// mutated after) touch no mutable state this mutex needs to protect,
// so they deliberately don't take it -- see their own comments.
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
    Lsn AppendUpdate(TxnId txn_id, ObjectId object_id, PageId page_id, const char* page_image);
    Lsn AppendCommit(TxnId txn_id);
    Lsn AppendAbort(TxnId txn_id);

    // fsyncs the WAL file. Every record appended so far becomes durable;
    // nothing appended after this call is covered until Flush() is called
    // again. Deliberately unguarded by mutex_: fsync only reads the
    // immutable fd_, and its "make durable whatever the OS has observed
    // so far" contract is safe to call concurrently with another
    // thread's AppendRecord -- a call from transaction Ti here is only
    // required to guarantee Ti's own already-appended (write()-returned)
    // records are durable, and program order on Ti's own calling thread
    // already ensures those writes happened-before this call.
    //
    // Always does its own unconditional fsync() call -- kept exactly as
    // simple and predictable as it always was, for callers (mainly
    // tests, and Database::Checkpoint()'s already-quiescent path, D-054)
    // that want one deterministic fsync with no coalescing. Concurrent
    // *transaction* commits should call FlushThrough below instead --
    // see its own comment for why this one doesn't scale under
    // concurrent load and what FlushThrough does about it (D-055).
    void Flush();

    // The group-commit-aware counterpart to Flush(), used by
    // TransactionManager::Commit() (docs/DECISIONS.md D-055). Blocks
    // until every record up through `target_lsn` (normally the caller's
    // own just-appended Commit record's LSN) is durable, but does not
    // guarantee this call itself performed the fsync that made it so --
    // if another thread is already mid-fsync when this one arrives, this
    // call simply waits for that fsync (or a subsequent one) to cover
    // `target_lsn`, rather than issuing a second, redundant fsync of its
    // own. The thread that *does* end up performing the fsync covers not
    // just its own target_lsn but every record appended by anyone up to
    // that moment (queried via NextLsn() immediately before the fsync
    // call), so commits that arrive while a flush is already in flight
    // get folded into it for free rather than each paying their own
    // fsync latency -- the whole point of group commit, and directly
    // motivated by D-055's benchmark finding that per-transaction fsync
    // dominates small-transaction latency, and that the resulting long
    // per-commit critical section (Strict 2PL holds every lock,
    // including BPlusTree's root-lock sentinel, until commit -- D-028)
    // was turning concurrent structural index operations into a
    // wait-die abort storm.
    //
    // Safe to call concurrently from many threads (unlike Flush(), which
    // relies on the caller's own program order to make its "only my own
    // writes need to be covered" reasoning hold -- FlushThrough instead
    // establishes that ordering itself, via flush_mutex_/flush_cv_).
    // Rethrows whatever the underlying fsync() failure produces (the
    // same error Flush() would) to every thread waiting on the flush
    // that failed, not just the one that happened to be doing it --
    // each waiter that doesn't see durable_lsn_ reach its own target
    // loops back and becomes the next flusher itself, so a transient
    // fsync failure doesn't strand anyone waiting forever on a flush
    // that already gave up.
    void FlushThrough(Lsn target_lsn);

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

    // The highest LSN FlushThrough has confirmed durable so far (0 if
    // FlushThrough has never been called). Exposed for tests -- mirrors
    // NextLsn()'s own reasoning for existing (D-055): a way to observe
    // FlushThrough's effect directly rather than only inferring it
    // indirectly through whether a later call blocks or returns.
    Lsn DurableLsn() const;

 private:
    Lsn AppendRecord(LogRecordType type, TxnId txn_id, ObjectId object_id, PageId page_id, const char* payload,
                      uint32_t payload_len);

    int fd_;
    Lsn next_lsn_;
    std::vector<LogRecord> records_on_open_;
    mutable std::mutex mutex_;

    // Group-commit state for FlushThrough (D-055) -- deliberately a
    // separate mutex from mutex_ above (which guards next_lsn_ and each
    // AppendRecord call's ordering), so a thread waiting on a flush to
    // complete never blocks a concurrent AppendRecord, and vice versa.
    // durable_lsn_ is the highest LSN confirmed fsync'd so far;
    // flush_in_progress_ is true exactly while some thread is between
    // releasing flush_mutex_ to call fsync() and reacquiring it to
    // publish the result -- see FlushThrough's own comment (this header)
    // for the full coalescing protocol.
    mutable std::mutex flush_mutex_;
    std::condition_variable flush_cv_;
    Lsn durable_lsn_ = 0;
    bool flush_in_progress_ = false;
};

}  // namespace flintdb
