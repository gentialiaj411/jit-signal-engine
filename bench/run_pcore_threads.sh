#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
LOG=bench/results/pcores_t246.log
: > "$LOG"
for T in 2 4 6; do
  echo "--- threads=$T ---" | tee -a "$LOG"
  taskset -c 1,2,3,4,5,6,7 build-wsl/multithread_scaling_benchmark --symbols 10000 --seconds 60 \
    --pin-core-base 1 --threads "$T" 2>&1 | tee -a "$LOG"
done
