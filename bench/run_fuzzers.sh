#!/usr/bin/env bash
# P8: drive the libFuzzer harnesses for a configurable amount of time.
#
# Build prerequisite (one-time):
#   mkdir -p build-fuzz && cd build-fuzz
#   CC=clang CXX=clang++ cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#                                  -DJITSE_BUILD_FUZZERS=ON
#   cmake --build . --target parser_fuzzer runtime_fuzzer -- -j
#
# Usage:
#   bench/run_fuzzers.sh [SECONDS=3600] [BUILD_DIR=build-fuzz]
#
# The script runs each harness for SECONDS wall time and writes
# discovery logs to bench/results/fuzz/<harness>_<timestamp>.log. Any
# crashing input found by libFuzzer is left in the corpus directory so
# it can be replayed deterministically by the standalone smoke
# harnesses (`parser_fuzzer_smoke`, `runtime_fuzzer_smoke`).

set -euo pipefail

SECONDS_PER_HARNESS="${1:-3600}"
BUILD_DIR="${2:-build-fuzz}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS_DIR="$ROOT/fuzz/corpus"
OUT_DIR="$ROOT/bench/results/fuzz"
mkdir -p "$OUT_DIR"

TS="$(date -u +%Y%m%dT%H%M%SZ)"

run_one() {
  local harness="$1"
  local bin="$ROOT/$BUILD_DIR/$harness"
  if [[ ! -x "$bin" ]]; then
    echo "[run_fuzzers] $harness not built at $bin; skipping." >&2
    return 1
  fi
  local log="$OUT_DIR/${harness}_${TS}.log"
  echo "[run_fuzzers] $harness -> $log ($SECONDS_PER_HARNESS s)"
  # detect_leaks=0 because LLVM caches some allocations across runs
  # that are not actual leaks. dedup_token_length=3 helps libFuzzer
  # bucket crashes by minimized stack trace prefix.
  ASAN_OPTIONS=detect_leaks=0 \
    "$bin" -max_total_time="$SECONDS_PER_HARNESS" \
           -print_final_stats=1 \
           "$CORPUS_DIR" 2>&1 | tee "$log"
}

run_one parser_fuzzer
run_one runtime_fuzzer

echo "[run_fuzzers] done; results in $OUT_DIR"
