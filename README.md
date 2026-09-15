# FlintDB

![CI](https://github.com/Sam-Haokip/flintdb/actions/workflows/ci.yml/badge.svg)

A single-node, embedded relational database engine, built from scratch in C++ — own page storage, own on-disk B+-tree, own write-ahead log and crash recovery, own transaction/concurrency control, and a small SQL front end, verified against real SQLite with differential testing.

This is a work in progress, built phase by phase, with a genuinely correct and tested system at the end of every phase. See:

- [`docs/SPEC.md`](docs/SPEC.md) — the exact SQL subset, durability and isolation guarantees, and explicit non-goals.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every real design decision made so far, the alternative it beat, and why.
- [`docs/BENCHMARK_PHASE2.md`](docs/BENCHMARK_PHASE2.md) — sequential scan vs. B+-tree index lookup, measured across table sizes.

A full README (problem statement, architecture, measured results, limitations) lands in the final packaging phase. Right now this is Phase 3 of 8: on top of Phase 1's storage foundation (fixed-size pages, a no-eviction buffer pool, heap-file row storage) and Phase 2's on-disk B+-tree secondary index, a write-ahead log and crash recovery — physical whole-page-image redo logging, a minimal single-active-transaction model, and a buffer-management policy (no-steal + force) chosen specifically to keep transaction abort correct with no undo log.

Building the B+-tree's delete path correctly (specifically, keeping split/merge/redistribution safe under duplicate keys without ever overflowing a page) took three real attempts before it was right; see D-012 through D-014 in the decisions log for the full story, including the bugs each earlier attempt had and how they were found.

Phase 3 found its own real bug the same way: an initial no-steal/no-force design (commit only writes the WAL, the data file catches up later) turned out to let one transaction's abort silently erase an *earlier, already-committed* transaction's change from memory, because abort reverts a page by re-reading the data file, and that file isn't guaranteed to be caught up yet under no-force. See D-020 for the fix (commit now force-flushes its touched pages) and the regression test that pins it down.

### Durability, tested the way the spec requires

Every durability claim below is backed by an actual test that performs real file I/O and, for the crash-simulation ones, manually truncates or bit-flips real on-disk bytes with raw POSIX calls before reopening fresh objects over the result — not an inspection of the code. See `docs/SPEC.md` §5 for the standard this follows, and `tests/test_log_manager.cpp`, `tests/test_recovery.cpp`, `tests/test_transaction.cpp`, and `tests/test_crash_recovery.cpp` for the tests themselves. Covered directly: a torn WAL tail after a complete record, a torn very first record, a structurally-complete-but-corrupted record (checksum catches it), a WAL torn specifically mid-Commit-record (the realistic "crash-mid-commit" case), a committed transaction whose data-file write hadn't happened yet, an uncommitted transaction's changes never appearing after recovery, idempotent recovery (replaying the same WAL twice is a safe no-op), a checkpoint followed by a crash before the next one, and a full HeapFile + transaction + crash + recovery integration run.

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
