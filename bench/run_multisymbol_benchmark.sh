#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-./build-wsl}"
OUT_CSV="${2:-./bench/results_multisymbol.csv}"
PASSES="${3:-2000}"

BENCH="$BUILD_DIR/multisymbol_benchmark"
if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found at $BENCH"
  exit 1
fi

"$BENCH" "$PASSES" "$OUT_CSV"
echo "Results written to: $OUT_CSV"
