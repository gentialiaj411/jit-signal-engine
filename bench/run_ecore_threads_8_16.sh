#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CORES=8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
LOG=bench/results/ecores_t8_16.log
: > "$LOG"
for T in 8 16; do
  echo "--- threads=$T ---" | tee -a "$LOG"
  taskset -c "$CORES" build-wsl/multithread_scaling_benchmark --symbols 10000 --seconds 60 \
    --pin-core-base 8 --threads "$T" 2>&1 | tee -a "$LOG"
done
cat "$LOG" >> bench/results/multithread_scaling_ecores_run.log
