# Phase 2 benchmark: sequential scan vs. B+-tree index lookup

Methodology: for each table size N, 200 point lookups on keys known to exist, each timed individually with `std::chrono::high_resolution_clock` inside a single process (structure-build cost excluded from the timed region). Sequential scan stops at the first match (`HeapFile::Find`) -- it is not a full-table materialization, so this is a fair comparison against what a real `WHERE key = ?` scan does. Rows inserted in random key order per table (not sorted), fixed random seed (42) for reproducibility.

Run: `cmake --build build && python3 scripts/bench_index_vs_scan.py`

| N | scan mean (µs) | scan stdev | index mean (µs) | index stdev | speedup |
|---:|---:|---:|---:|---:|---:|
| 100 | 29.1 | 16.8 | 3.40 | 3.50 | 8.6x |
| 1000 | 290.8 | 187.9 | 7.82 | 2.62 | 37.2x |
| 10000 | 2915.8 | 1796.0 | 7.43 | 3.27 | 392.2x |
| 100000 | 32184.3 | 17578.2 | 13.77 | 4.41 | 2338.1x |
