# FlintDB

![CI](https://github.com/Sam-Haokip/flintdb/actions/workflows/ci.yml/badge.svg)

A single-node, embedded relational database engine, built from scratch in C++ — own page storage, own on-disk B+-tree, own write-ahead log and crash recovery, own transaction/concurrency control, and a small SQL front end, verified against real SQLite with differential testing.

This is a work in progress, built phase by phase, with a genuinely correct and tested system at the end of every phase. See:

- [`docs/SPEC.md`](docs/SPEC.md) — the exact SQL subset, durability and isolation guarantees, and explicit non-goals.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every real design decision made so far, the alternative it beat, and why.
- [`docs/BENCHMARK_PHASE2.md`](docs/BENCHMARK_PHASE2.md) — sequential scan vs. B+-tree index lookup, measured across table sizes.

A full README (problem statement, architecture, measured results, limitations) lands in the final packaging phase. Right now this is Phase 2 of 8: on top of Phase 1's storage foundation (fixed-size pages, a no-eviction buffer pool, heap-file row storage), an on-disk B+-tree secondary index — non-unique keys, splits, merges, and redistribution, all sharing the buffer pool with the heap file.

Building the B+-tree's delete path correctly (specifically, keeping split/merge/redistribution safe under duplicate keys without ever overflowing a page) took three real attempts before it was right; see D-012 through D-014 in the decisions log for the full story, including the bugs each earlier attempt had and how they were found.

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
