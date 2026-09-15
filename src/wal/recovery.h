#pragma once
#include "../storage/disk_manager.h"
#include "log_record.h"

#include <cstddef>
#include <vector>

namespace flintdb {

// Replays a WAL onto the data file after a crash (or a clean shutdown --
// running recovery against an already-consistent data file is required
// to be a safe no-op, since nothing calls this conditionally on whether
// a crash actually happened).
//
// Redo-only, per FlintDB's no-steal buffer pool policy (docs/SPEC.md
// section 2): a dirty page is never written to the data file before its
// transaction commits (see transaction_manager.h), so a page on disk can
// never hold a *partial* transaction's changes -- there is nothing to
// undo, ever. Recovery's only job is to finish the work of transactions
// that committed but whose pages might not have reached the data file
// before the crash.
//
// Two passes over `records`, which must already be in LSN order --
// exactly what LogManager::RecordsOnOpen() returns, and the only thing
// this is ever called with in practice:
//   1. Collect every txn_id that has a kCommit record. A transaction is
//      "committed" if and only if its Commit record made it into the
//      log -- a crash between writing a transaction's Update records and
//      writing its Commit record leaves those Update records in the log
//      with no matching Commit, and pass 2 correctly ignores them.
//   2. Replay every kUpdate record belonging to a committed txn_id, in
//      LSN order, via DiskManager::WritePage. Each one means exactly
//      "page_id becomes this image, in full", so replaying the same
//      record more than once (recovery runs, the process dies again
//      before the next checkpoint, and recovery runs again over a WAL
//      that still has the same records) is idempotent by construction:
//      every replay just writes the same bytes again.
//
// kBegin and kAbort records need no action in either pass: kBegin exists
// only so a future extension (e.g. Phase 4's concurrency work) has
// somewhere to hang per-transaction bookkeeping if it needs it, and
// TransactionManager::Abort (transaction_manager.h) never logs kUpdate
// records for the transaction it's aborting in the first place -- it
// discards those pages from the buffer pool instead -- so an aborted
// transaction's txn_id simply never has any kUpdate records to skip.
//
// Returns the number of kUpdate records actually replayed (0 if there
// was nothing to redo), so a caller can log or assert on it without
// duplicating this function's two passes.
//
// Known gap (see docs/DECISIONS.md): this only makes committed *page
// content* durable and redoable. Page *allocation* (DiskManager::
// AllocatePage growing the file) happens immediately and unconditionally,
// outside the WAL, so it isn't itself covered by this recovery guarantee
// -- an allocated-but-never-committed page is simply wasted space, never
// a correctness problem, since nothing indexes a page until a committed
// transaction says so.
size_t RunRecovery(const std::vector<LogRecord>& records, DiskManager* disk_manager);

}  // namespace flintdb
