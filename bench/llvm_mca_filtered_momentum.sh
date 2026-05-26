#!/usr/bin/env bash
# P9: capture the host-target assembly of the fused filtered_momentum
# program and run it through `llvm-mca` to predict IPC, port pressure,
# and cycle counts for the steady-state warmed-loop iteration. The
# resulting analysis is written to
# bench/results/llvm_mca/filtered_momentum.txt and checked in.
#
# Why llvm-mca: it reads textual assembly and reports per-instruction
# scheduling and throughput estimates against a CPU model. For a JIT
# that targets the local machine, this gives us a cycle-level view of
# what the LLVM backend produced, without needing a hardware
# performance counter or a microbenchmark harness. It is *predicted*
# rather than measured, so we use it as evidence of "the compiler
# emitted code with reasonable port balance" rather than as a
# benchmark substitute.
#
# Usage:
#   bench/llvm_mca_filtered_momentum.sh           # uses default x86-64 model
#   bench/llvm_mca_filtered_momentum.sh skylake   # specific CPU model
#
# Build prerequisite: any in-tree build of `jit_signal_engine`. The
# script picks the first that exists from a small candidate list.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Find a usable build directory.
BUILD_DIR=""
for candidate in build-wsl build build-fuzz; do
  if [[ -x "$ROOT/$candidate/jit_signal_engine" ]]; then
    BUILD_DIR="$ROOT/$candidate"
    break
  fi
done
if [[ -z "$BUILD_DIR" ]]; then
  echo "llvm_mca: jit_signal_engine binary not found in build*/." >&2
  echo "Configure + build first: cmake --build build-wsl --target jit_signal_engine" >&2
  exit 2
fi

# Pick an llvm-mca. Prefer the highest installed major version because
# its CPU models track recent silicon best.
MCA=""
for v in 21 20 19 18 17 16 ""; do
  if command -v "llvm-mca${v:+-$v}" >/dev/null 2>&1; then
    MCA="llvm-mca${v:+-$v}"
    break
  fi
done
if [[ -z "$MCA" ]]; then
  echo "llvm_mca: no llvm-mca binary on PATH; install llvm-tools." >&2
  exit 3
fi

OUT_DIR="$ROOT/bench/results/llvm_mca"
mkdir -p "$OUT_DIR"

ASM_FILE="$OUT_DIR/filtered_momentum.s"
MCA_FILE="$OUT_DIR/filtered_momentum.txt"
SIG_FILE="$ROOT/examples/filtered_momentum.sig"

echo "[llvm_mca] dumping asm from $BUILD_DIR/jit_signal_engine ..."
"$BUILD_DIR/jit_signal_engine" --dump-asm --all-signals "$SIG_FILE" 100 \
  >/dev/null 2>"$ASM_FILE"

CPU_FLAG=()
if [[ $# -ge 1 ]]; then
  CPU_FLAG=("-mcpu=$1")
fi

echo "[llvm_mca] analyzing with $MCA ${CPU_FLAG[*]} ..."
{
  echo "; llvm-mca analysis of the fused filtered_momentum program"
  echo "; Asm captured from jit_signal_engine --dump-asm"
  echo "; mca: $MCA ${CPU_FLAG[*]}"
  echo "; date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "; host: $(uname -mrs)"
  echo ""
  "$MCA" "${CPU_FLAG[@]}" "$ASM_FILE"
} > "$MCA_FILE"

echo "[llvm_mca] wrote $MCA_FILE ($(wc -l < "$MCA_FILE") lines)"
