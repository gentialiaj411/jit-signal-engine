#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-./build-wsl}"
OUT_CSV="${2:-./bench/results_simd.csv}"
EVENTS="${3:-1000000}"

BENCH="$BUILD_DIR/simd_benchmark"
if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found at $BENCH"
  exit 1
fi

"$BENCH" "$EVENTS" "$OUT_CSV"
echo "Results written to: $OUT_CSV"
