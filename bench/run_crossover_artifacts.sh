#!/usr/bin/env bash
# P5 artifact generator. Runs compile_runtime_crossover on the canonical
# signals and writes per-signal {csv, md, svg} into bench/results/crossover/.
#
# Usage:
#   bench/run_crossover_artifacts.sh       # default sample sizes
#
# Run from project root.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-wsl}"
OUT_DIR="${OUT_DIR:-bench/results/crossover}"
EVENTS="${EVENTS:-200000}"
COMPILE_RUNS="${COMPILE_RUNS:-7}"
EVENT_RUNS="${EVENT_RUNS:-5}"

mkdir -p "$OUT_DIR"

SIGS=(spread_signal filtered_momentum profile_canonical)

for sig in "${SIGS[@]}"; do
  echo "=== $sig ==="
  "$BUILD_DIR/compile_runtime_crossover" "examples/${sig}.sig" \
      --events="$EVENTS" \
      --compile-runs="$COMPILE_RUNS" \
      --event-runs="$EVENT_RUNS" \
      --out-dir="$OUT_DIR"
done
