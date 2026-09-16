#pragma once
#include "../common/config.h"

#include <array>
#include <cstdint>

namespace flintdb {

// A transaction id. Assigned by whoever begins transactions (Phase 3 keeps
// this trivial -- see transaction_manager.h); the WAL itself just treats
// it as an opaque tag it doesn't interpret.
using TxnId = uint64_t;

// A log sequence number: the position of a record in the WAL, assigned by
// LogManager strictly in append order starting at 1. LSNs are only ever
// compared for ordering (RunRecovery replays kUpdate records in LSN
// order) -- nothing about their numeric value is meaningful beyond that.
using Lsn = uint64_t;

// Every record type FlintDB's WAL can hold. There is deliberately no
// "checkpoint" record type: Phase 3's checkpoint (see LogManager::Checkpoint)
// is "quiescent" -- it flushes every dirty page via BufferPool::FlushAll()
// and then wipes the WAL file to empty, so there is never a surviving log
// record for recovery to treat specially. See docs/DECISIONS.md D-018.
enum class LogRecordType : uint8_t {
    kBegin = 1,
    kUpdate = 2,
    kCommit = 3,
    kAbort = 4,
};

// One record in the write-ahead log, as handed back by
// LogManager::RecordsOnOpen() for RunRecovery to replay.
//
// kUpdate is a *physical, whole-page-image* redo record: "page_id becomes
// exactly page_image, in full" -- not a byte-range diff, and not an undo
// record (FlintDB's no-steal buffer pool policy means recovery never
// needs to undo anything; see docs/SPEC.md section 2 and
// docs/DECISIONS.md D-016 for why physical whole-page logging was chosen
// over physiological byte-range logging).
//
// kBegin/kCommit/kAbort don't use object_id, page_id, or page_image; those
// fields are left at their defaults for those record types. Every record
// type uses this same struct shape rather than a tagged union or per-type
// subclass, which wastes 4KB per non-update record in memory -- an
// acceptable tradeoff for how small a real WAL's Begin/Commit/Abort record
// count is next to its Update record count, and it keeps RunRecovery's
// replay loop (recovery.h) a single flat switch over one record shape.
//
// object_id (Phase 5, docs/DECISIONS.md D-032): which table/index's file
// page_id refers to. A bare page_id is only unique within one object's own
// file (docs/DECISIONS.md D-031 -- every table/index gets its own
// DiskManager-backed file), so recovery needs to know which file to replay
// an Update record's page_id against; RunRecovery takes an
// ObjectId -> DiskManager* map for exactly this reason (recovery.h).
struct LogRecord {
    Lsn lsn = 0;
    LogRecordType type = LogRecordType::kBegin;
    TxnId txn_id = 0;
    ObjectId object_id = INVALID_OBJECT_ID;
    PageId page_id = INVALID_PAGE_ID;
    std::array<char, PAGE_SIZE> page_image{};  // only meaningful for kUpdate
};

}  // namespace flintdb
