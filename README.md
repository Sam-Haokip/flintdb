# FlintDB

![CI](https://github.com/Sam-Haokip/flintdb/actions/workflows/ci.yml/badge.svg)

A single-node, embedded relational database engine, built from scratch in C++ — own page storage, own on-disk B+-tree, own write-ahead log and crash recovery, own transaction/concurrency control, and a small SQL front end, verified against real SQLite with differential testing.

This is a work in progress, built phase by phase, with a genuinely correct and tested system at the end of every phase. See:

- [`docs/SPEC.md`](docs/SPEC.md) — the exact SQL subset, durability and isolation guarantees, and explicit non-goals.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every real design decision made so far, the alternative it beat, and why.

A full README (problem statement, architecture, measured results, limitations) lands in the final packaging phase. Right now this is the storage foundation — Phase 1 of 8: a fixed-size page format, a no-eviction buffer pool, and heap-file row storage (append / scan / delete-by-rewrite).

## Building and testing

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler and CMake 3.16+.
