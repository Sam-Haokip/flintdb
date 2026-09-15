# FlintDB

![CI](https://github.com/Sam-Haokip/flintdb/actions/workflows/ci.yml/badge.svg)

A single-node, embedded relational database engine, built from scratch in C++ — own page storage, own on-disk B+-tree, own write-ahead log and crash recovery, own transaction/concurrency control, and a small SQL front end, verified against real SQLite with differential testing.

This is a work in progress, built phase by phase, with a genuinely correct and tested system at the end of every phase. See:

- [`docs/SPEC.md`](docs/SPEC.md) — the exact SQL subset, durability and isolation guarantees, and explicit non-goals.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every real design decision made so far, the alternative it beat, and why.
- [`docs/BENCHMARK_PHASE2.md`](docs/BENCHMARK_PHASE2.md) — sequential scan vs. B+-tree index lookup, measured across table sizes.

A full README (problem statement, architecture, measured results, limitations) lands in the final packaging phase. Right now this is Phase 4 of 8: on top of Phase 1's storage foundation (fixed-size pages, a no-eviction buffer pool, heap-file row storage), Phase 2's on-disk B+-tree secondary index, and Phase 3's write-ahead log and crash recovery, Phase 4 adds real multithreaded transactions and concurrency control — Strict Two-Phase Locking at page granularity, wait-die deadlock prevention, and a thread-per-transaction model where multiple transactions genuinely run at once on their own `std::thread`s, not a single-active-transaction stand-in.

Building the B+-tree's delete path correctly (specifically, keeping split/merge/redistribution safe under duplicate keys without ever overflowing a page) took three real attempts before it was right; see D-012 through D-014 in the decisions log for the full story, including the bugs each earlier attempt had and how they were found.

Phase 3 found its own real bug the same way: an initial no-steal/no-force design (commit only writes the WAL, the data file catches up later) turned out to let one transaction's abort silently erase an *earlier, already-committed* transaction's change from memory, because abort reverts a page by re-reading the data file, and that file isn't guaranteed to be caught up yet under no-force. See D-020 for the fix (commit now force-flushes its touched pages) and the regression test that pins it down.

Phase 4 found two more bugs the same way, before either one ever reached a failing test: lifting the single-active-transaction restriction meant `BufferPool::FlushAll()` inside `Commit()` would also flush a *different*, still-uncommitted transaction's dirty pages — a no-steal violation caught by asking what, precisely, changes once more than one transaction can be active (D-027, fixed by scoping the flush to `FlushPages(txn->DirtiedPages())`). And the phantom-read gap this phase's own `docs/SPEC.md` originally described turned out to be wrong once actually reproduced: a full-table `Scan()` turns out to fully close that gap, as an accidental side effect of how `HeapFile::Insert()` locks pages; the real, demonstrated gap is `HeapFile::Find()`'s early return leaving later pages unlocked (D-023) — a correction made *before* the isolation tests shipped, not after one failed.

### Durability, tested the way the spec requires

Every durability claim below is backed by an actual test that performs real file I/O and, for the crash-simulation ones, manually truncates or bit-flips real on-disk bytes with raw POSIX calls before reopening fresh objects over the result — not an inspection of the code. See `docs/SPEC.md` §5 for the standard this follows, and `tests/test_log_manager.cpp`, `tests/test_recovery.cpp`, `tests/test_transaction.cpp`, and `tests/test_crash_recovery.cpp` for the tests themselves. Covered directly: a torn WAL tail after a complete record, a torn very first record, a structurally-complete-but-corrupted record (checksum catches it), a WAL torn specifically mid-Commit-record (the realistic "crash-mid-commit" case), a committed transaction whose data-file write hadn't happened yet, an uncommitted transaction's changes never appearing after recovery, idempotent recovery (replaying the same WAL twice is a safe no-op), a checkpoint followed by a crash before the next one, and a full HeapFile + transaction + crash + recovery integration run.

### Isolation, tested the way the spec requires

Every isolation claim is backed by a named test that reproduces — or, for the one documented gap, precisely bounds — the specific anomaly, driving real `HeapFile` + `TransactionManager` traffic across genuinely concurrent `std::thread`s (not simulated interleaving on one thread). See `docs/SPEC.md` §3 for the guarantee and the full anomaly table, and `tests/test_isolation.cpp` for the tests. Covered: dirty reads (a reader blocks on a writer's uncommitted exclusive lock), lost updates (a second read-modify-write can't even read until the first one commits, so its increment always lands on top of, not instead of, the first), non-repeatable reads (a reader's two reads within one transaction are always identical, because its shared lock is held for the whole transaction, not released between reads), and phantom reads — prevented when a full-table scan has already locked every existing page, and honestly *not* prevented (with a test proving the gap, not just describing it) when an early-returning `Find()` never had to lock a later page at all. Every concurrency claim in this phase is additionally verified clean under ThreadSanitizer, not just a normal build — see D-025 and D-030 for what that caught along the way.

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
