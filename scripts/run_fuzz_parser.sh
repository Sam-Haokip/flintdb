#!/usr/bin/env bash
# Builds and runs the lexer/parser libFuzzer target (task #53,
# docs/DECISIONS.md D-053). Not part of the main CMake build -- see
# src/sql/fuzz_parser.cpp's own top comment for why libFuzzer needs Clang
# specifically, not whatever compiler CMakeLists.txt's default build picks
# up (g++ in this environment).
#
# Usage:
#   scripts/run_fuzz_parser.sh [max_total_time_seconds]
#
# Default run length is short (suited to a local sanity check or a CI
# step, task #55); pass a larger number for a real fuzzing session, e.g.:
#   scripts/run_fuzz_parser.sh 300
#
# On a crash, libFuzzer writes its own reproducer file (crash-<hash>) into
# the current directory and this script exits non-zero -- rerun the fuzz
# binary directly on that one file (build/fuzz_parser crash-<hash>) to
# reproduce it deterministically outside the fuzzing loop.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

MAX_TOTAL_TIME="${1:-30}"

mkdir -p build-fuzz
clang++ -std=c++20 -g -O1 \
  -fsanitize=fuzzer,address,undefined \
  -Isrc \
  src/sql/fuzz_parser.cpp \
  src/sql/parser.cpp \
  src/sql/lexer.cpp \
  src/sql/token.cpp \
  -o build-fuzz/fuzz_parser

mkdir -p build-fuzz/corpus
./build-fuzz/fuzz_parser -max_total_time="$MAX_TOTAL_TIME" build-fuzz/corpus
