#!/usr/bin/env bash
# Run all signal benchmarks in Release mode and write results to CSV.
# Requires: Linux + LLVM available at cmake configure time for JIT numbers.
# Usage: bash bench/run_benchmarks.sh [build_dir] [out_csv] [pin_core] [events]
set -euo pipefail

BUILD_DIR="${1:-./build/Release}"
OUT_CSV="${2:-./bench/results.csv}"
PIN_CORE="${3:-0}"
EVENTS="${4:-1000000}"

BENCH="$BUILD_DIR/signal_benchmark"
if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found at $BENCH"
  echo "Build with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

rm -f "$OUT_CSV"

PIN_ARGS=(--pin-core "$PIN_CORE")

# Single-signal runs.
"$BENCH" "${PIN_ARGS[@]}" ./examples/spread_signal.sig         "$EVENTS" "$OUT_CSV" spread
"$BENCH" "${PIN_ARGS[@]}" ./examples/momentum_signal.sig        "$EVENTS" "$OUT_CSV" momentum
"$BENCH" "${PIN_ARGS[@]}" ./examples/zscore_signal.sig          "$EVENTS" "$OUT_CSV" spread_z
"$BENCH" "${PIN_ARGS[@]}" ./examples/zscore_builtin_signal.sig  "$EVENTS" "$OUT_CSV" z
"$BENCH" "${PIN_ARGS[@]}" ./examples/vwap_signal.sig            "$EVENTS" "$OUT_CSV" dev

# All-signals run (CompileProgram / eval_all path).
"$BENCH" "${PIN_ARGS[@]}" --all-signals ./examples/filtered_momentum.sig "$EVENTS" "$OUT_CSV"

echo ""
echo "Results written to: $OUT_CSV"
echo "Check jit_mode column: must be 'enabled' for JIT numbers to be valid."
