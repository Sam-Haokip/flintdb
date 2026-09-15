#!/usr/bin/env python3
"""Phase 2 benchmark harness.

Runs the bench_index_vs_scan C++ binary at several table sizes, collects
per-lookup timing samples, and reports mean/stdev for sequential scan vs.
B+-tree index lookup -- per the project's standing rule that every
performance claim needs repeated measurements with variance, not a
single run.

Usage: python3 scripts/bench_index_vs_scan.py   (after cmake --build build)
"""
import csv
import io
import statistics
import subprocess
import sys
from pathlib import Path

BINARY = Path(__file__).resolve().parent.parent / "build" / "bench_index_vs_scan"
TABLE_SIZES = [100, 1_000, 10_000, 100_000]
LOOKUPS_PER_SIZE = 200
SEED = 42


def run_one(n: int, lookups: int, seed: int):
    proc = subprocess.run(
        [str(BINARY), str(n), str(lookups), str(seed)],
        capture_output=True, text=True, check=True,
    )
    reader = csv.DictReader(io.StringIO(proc.stdout))
    scan_samples, index_samples = [], []
    for row in reader:
        micros = int(row["microseconds"])
        (scan_samples if row["method"] == "scan" else index_samples).append(micros)
    return scan_samples, index_samples


def summarize(samples):
    return {
        "mean_us": statistics.mean(samples),
        "stdev_us": statistics.stdev(samples) if len(samples) > 1 else 0.0,
        "min_us": min(samples),
        "max_us": max(samples),
    }


def main():
    if not BINARY.exists():
        print(f"error: {BINARY} not found -- build the project first (cmake --build build)", file=sys.stderr)
        sys.exit(1)

    rows = []
    for n in TABLE_SIZES:
        scan_samples, index_samples = run_one(n, LOOKUPS_PER_SIZE, SEED)
        scan_stats = summarize(scan_samples)
        index_stats = summarize(index_samples)
        speedup = scan_stats["mean_us"] / index_stats["mean_us"] if index_stats["mean_us"] > 0 else float("inf")
        rows.append((n, scan_stats, index_stats, speedup))
        print(
            f"n={n:>7}  scan: {scan_stats['mean_us']:>10.1f} us (+/- {scan_stats['stdev_us']:>8.1f})   "
            f"index: {index_stats['mean_us']:>8.2f} us (+/- {index_stats['stdev_us']:>6.2f})   "
            f"speedup: {speedup:>8.1f}x",
            flush=True,
        )

    report_path = Path(__file__).resolve().parent.parent / "docs" / "BENCHMARK_PHASE2.md"
    with open(report_path, "w") as f:
        f.write("# Phase 2 benchmark: sequential scan vs. B+-tree index lookup\n\n")
        f.write(
            f"Methodology: for each table size N, {LOOKUPS_PER_SIZE} point lookups on keys known to exist, "
            f"each timed individually with `std::chrono::high_resolution_clock` inside a single process "
            f"(structure-build cost excluded from the timed region). Sequential scan stops at the first "
            f"match (`HeapFile::Find`) -- it is not a full-table materialization, so this is a fair "
            f"comparison against what a real `WHERE key = ?` scan does. Rows inserted in random key order "
            f"per table (not sorted), fixed random seed ({SEED}) for reproducibility.\n\n"
            f"Run: `cmake --build build && python3 scripts/bench_index_vs_scan.py`\n\n"
        )
        f.write("| N | scan mean (µs) | scan stdev | index mean (µs) | index stdev | speedup |\n")
        f.write("|---:|---:|---:|---:|---:|---:|\n")
        for n, scan_stats, index_stats, speedup in rows:
            f.write(
                f"| {n} | {scan_stats['mean_us']:.1f} | {scan_stats['stdev_us']:.1f} | "
                f"{index_stats['mean_us']:.2f} | {index_stats['stdev_us']:.2f} | {speedup:.1f}x |\n"
            )
    print(f"\nReport written to {report_path}")


if __name__ == "__main__":
    main()
