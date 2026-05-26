#!/usr/bin/env bash
# P4 artifact generator. Runs latency_bench on the three canonical signals
# and writes the per-signal {csv, md, svg} into bench/results/latency/.
#
# Usage:
#   ./bench/run_latency_artifacts.sh                # closed-loop only
#   ./bench/run_latency_artifacts.sh --co           # also CO-aware runs
#
# Run from the project root.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-wsl}"
OUT_DIR="${OUT_DIR:-bench/results/latency}"
EVENTS="${EVENTS:-1000000}"
WARMUP="${WARMUP:-50000}"
DO_CO=0
if [[ "${1-}" == "--co" ]]; then DO_CO=1; fi

mkdir -p "$OUT_DIR"

SIGS=(spread_signal filtered_momentum profile_canonical)

for sig in "${SIGS[@]}"; do
  echo "=== $sig (closed-loop) ==="
  "$BUILD_DIR/latency_bench" "examples/${sig}.sig" \
      --events="$EVENTS" --warmup="$WARMUP" \
      --out-dir="$OUT_DIR"
done

if [[ "$DO_CO" -eq 1 ]]; then
  # CO-aware rates chosen at ~60% of each signal's closed-loop JIT
  # throughput so the rate gate is reachable but tight enough to reveal
  # OS scheduling jitter as real tail latency.
  declare -A CO_RATES=(
    [spread_signal]=20000000
    [filtered_momentum]=4000000
    [profile_canonical]=3000000
  )
  for sig in "${SIGS[@]}"; do
    rate="${CO_RATES[$sig]}"
    out="$OUT_DIR/co_$((rate / 1000000))mhz"
    mkdir -p "$out"
    echo "=== $sig (CO-aware @ ${rate} Hz, JIT only) ==="
    "$BUILD_DIR/latency_bench" "examples/${sig}.sig" \
        --events=400000 --warmup=20000 --rate-hz="$rate" --jit-only \
        --out-dir="$out"
  done
fi
