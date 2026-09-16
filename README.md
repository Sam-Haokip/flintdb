# FlintDB

![CI](https://github.com/Sam-Haokip/flintdb/actions/workflows/ci.yml/badge.svg)

A single-node, embedded relational database engine, built from scratch in C++ — own page storage, own on-disk B+-tree, own write-ahead log and crash recovery, own transaction/concurrency control, and a small SQL front end, verified against real SQLite with differential testing.

This is a work in progress, built phase by phase, with a genuinely correct and tested system at the end of every phase. See:

- [`docs/SPEC.md`](docs/SPEC.md) — the exact SQL subset, durability and isolation guarantees, and explicit non-goals.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every real design decision made so far, the alternative it beat, and why.
- [`docs/BENCHMARK_PHASE2.md`](docs/BENCHMARK_PHASE2.md) — sequential scan vs. B+-tree index lookup, measured across table sizes.

A full README (problem statement, architecture, measured results, limitations) lands in the final packaging phase. Right now this is Phase 5 of 8: on top of Phase 1's storage foundation (fixed-size pages, a no-eviction buffer pool, heap-file row storage), Phase 2's on-disk B+-tree secondary index, Phase 3's write-ahead log and crash recovery, and Phase 4's real multithreaded transactions and concurrency control (Strict Two-Phase Locking at page granularity, wait-die deadlock prevention, a thread-per-transaction model where multiple transactions genuinely run at once), Phase 5 adds a small SQL front end end to end: a hand-written lexer and recursive-descent parser for the SQL subset `docs/SPEC.md` §1 defines, a fixed-rule query planner (index scan vs. sequential scan, equi-join access-path selection, no cost-based optimizer — §4's non-goals), an executor that runs `CREATE TABLE`/`CREATE INDEX`/`INSERT`/`SELECT`/`UPDATE`/`DELETE` against the real storage engine underneath, and a transaction-lifecycle layer implementing `BEGIN`/`COMMIT`/`ROLLBACK` plus SQLite-style implicit autocommit for any statement run outside an explicit transaction.

Building the B+-tree's delete path correctly (specifically, keeping split/merge/redistribution safe under duplicate keys without ever overflowing a page) took three real attempts before it was right; see D-012 through D-014 in the decisions log for the full story, including the bugs each earlier attempt had and how they were found.

Phase 3 found its own real bug the same way: an initial no-steal/no-force design (commit only writes the WAL, the data file catches up later) turned out to let one transaction's abort silently erase an *earlier, already-committed* transaction's change from memory, because abort reverts a page by re-reading the data file, and that file isn't guaranteed to be caught up yet under no-force. See D-020 for the fix (commit now force-flushes its touched pages) and the regression test that pins it down.

Phase 4 found two more bugs the same way, before either one ever reached a failing test: lifting the single-active-transaction restriction meant `BufferPool::FlushAll()` inside `Commit()` would also flush a *different*, still-uncommitted transaction's dirty pages — a no-steal violation caught by asking what, precisely, changes once more than one transaction can be active (D-027, fixed by scoping the flush to `FlushPages(txn->DirtiedPages())`). And the phantom-read gap this phase's own `docs/SPEC.md` originally described turned out to be wrong once actually reproduced: a full-table `Scan()` turns out to fully close that gap, as an accidental side effect of how `HeapFile::Insert()` locks pages; the real, demonstrated gap is `HeapFile::Find()`'s early return leaving later pages unlocked (D-023) — a correction made *before* the isolation tests shipped, not after one failed.

Phase 5 found its own bugs too, several before a single test ever failed. Reopening a database whose index had just root-split turned into a three-attempt fix, not one: the first pass got the *order* of recovery vs. buffer-pool construction wrong (D-041), the second removed a stale in-memory root-pointer cache that the first pass's own reasoning had exposed as a second, independent hazard (D-042), and the third found that even with both fixed, a brand-new index's header page could still be reverted to raw zero bytes by an unrelated transaction's abort, because it had never actually been flushed to disk in the first place (D-043) — three distinct bugs sharing one symptom (a reopened or reverted B+-tree silently misreading its own root as a real node and looping forever), each caught by design reasoning or a `gdb` backtrace on a hung test process, never by inspection alone. Two more surfaced while building the planner and executor: `Catalog` had never actually enforced that only `INTEGER` columns can be indexed, even though the B+-tree has only ever supported `int64_t` keys (D-046), and `UPDATE`'s delete-then-reinsert had to be checked — and sabotage-tested, by deliberately reintroducing the bug and confirming the test catches it — to prove it reindexes *every* index on a table, not just the ones a `SET` clause happened to touch (D-048). The last was a real null-pointer hazard in the transaction manager's `Commit`/`Abort`, unreachable by any existing caller but exposed the moment `session.h` added a new one (a bare `COMMIT` with no prior `BEGIN`) — found by reading the guard condition itself rather than trusting its doc comment, and confirmed with a standalone ASan repro showing the actual SEGV before the fix went in (D-049).

### Durability, tested the way the spec requires

Every durability claim below is backed by an actual test that performs real file I/O and, for the crash-simulation ones, manually truncates or bit-flips real on-disk bytes with raw POSIX calls before reopening fresh objects over the result — not an inspection of the code. See `docs/SPEC.md` §5 for the standard this follows, and `tests/test_log_manager.cpp`, `tests/test_recovery.cpp`, `tests/test_transaction.cpp`, and `tests/test_crash_recovery.cpp` for the tests themselves. Covered directly: a torn WAL tail after a complete record, a torn very first record, a structurally-complete-but-corrupted record (checksum catches it), a WAL torn specifically mid-Commit-record (the realistic "crash-mid-commit" case), a committed transaction whose data-file write hadn't happened yet, an uncommitted transaction's changes never appearing after recovery, idempotent recovery (replaying the same WAL twice is a safe no-op), a checkpoint followed by a crash before the next one, and a full HeapFile + transaction + crash + recovery integration run.

### Isolation, tested the way the spec requires

Every isolation claim is backed by a named test that reproduces — or, for the one documented gap, precisely bounds — the specific anomaly, driving real `HeapFile` + `TransactionManager` traffic across genuinely concurrent `std::thread`s (not simulated interleaving on one thread). See `docs/SPEC.md` §3 for the guarantee and the full anomaly table, and `tests/test_isolation.cpp` for the tests. Covered: dirty reads (a reader blocks on a writer's uncommitted exclusive lock), lost updates (a second read-modify-write can't even read until the first one commits, so its increment always lands on top of, not instead of, the first), non-repeatable reads (a reader's two reads within one transaction are always identical, because its shared lock is held for the whole transaction, not released between reads), and phantom reads — prevented when a full-table scan has already locked every existing page, and honestly *not* prevented (with a test proving the gap, not just describing it) when an early-returning `Find()` never had to lock a later page at all. Every concurrency claim in this phase is additionally verified clean under ThreadSanitizer, not just a normal build — see D-025 and D-030 for what that caught along the way.

### SQL front end, tested layer by layer and end to end

Each stage of the pipeline has its own dedicated test file — `tests/test_lexer.cpp`, `tests/test_parser.cpp`, `tests/test_planner.cpp`, `tests/test_executor.cpp`, `tests/test_session.cpp` — exercising that one layer in isolation. `tests/test_end_to_end.cpp` adds what none of those do: realistic multi-table, multi-statement-type workflows driven entirely through raw SQL text (`Parse()` + `session.h`'s `Execute()`, no direct calls into any lower layer), and restart/crash durability checked from the SQL layer's own point of view. That last test builds a database up through a mix of autocommit statements, an explicit transaction committed after 320 inserts (enough to force a real `PRIMARY KEY` index root split), and a transaction that issues `BEGIN` and is then simply abandoned — no `COMMIT`, no `ROLLBACK` — which is what a real process crash mid-transaction actually looks like from outside the transaction itself. After reopening, everything genuinely committed must survive (including rows on the far side of the split, reached back through the reopened index) and the abandoned transaction's work must be entirely absent. 274 tests pass clean across a normal build, AddressSanitizer+UndefinedBehaviorSanitizer, and ThreadSanitizer.

## Building and testing

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler and CMake 3.16+.

## Benchmarking

```bash
cmake --build build
python3 scripts/bench_index_vs_scan.py
```

Builds a heap file and a B+-tree index over the same N rows, times point lookups against each, and writes `docs/BENCHMARK_PHASE2.md`.
