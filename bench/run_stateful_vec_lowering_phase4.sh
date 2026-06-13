#!/usr/bin/env bash
# Phase 4: three-way stateful lowering × vectorization benchmark.
# Usage: bash bench/run_stateful_vec_lowering_phase4.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build-wsl}"
BENCH="$BUILD_DIR/cross_symbol_benchmark"
OUT_MD="$ROOT/bench/results/stateful_vec_lowering_speedup.md"
SIG="$ROOT/examples/filtered_momentum.sig"
EVENTS=1000000
LANES=4
RUNS=5

if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found: $BENCH"
  exit 1
fi

echo "=== Phase 4: stateful vec + lowering benchmark ==="
echo "build=$BUILD_DIR program=$SIG events=$EVENTS lanes=$LANES runs=$RUNS"

"$BENCH" "$SIG" "$EVENTS" --lanes="$LANES" --runs="$RUNS" --phase4 --md="$OUT_MD"

echo "artifact=$OUT_MD"
