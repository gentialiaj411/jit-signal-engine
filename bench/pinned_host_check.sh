#!/usr/bin/env bash
# Validate pinned-host fingerprint (no benchmarks). Usage: bash bench/pinned_host_check.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-build-wsl}"
SPEC="$ROOT/bench/pinned_host_spec.json"

read_spec() { python3 -c "import json; print(json.load(open('$SPEC'))$1)"; }
fail() { echo "PINNED HOST CHECK FAILED: $*" >&2; exit 1; }

PIN_CORE="$(read_spec "['pin_core']")"

KERNEL="$(uname -r)"
KERNEL_NEED="$(read_spec "['os']['kernel_substring']")"
grep -qF "$KERNEL_NEED" <<<"$KERNEL" || fail "kernel $KERNEL missing substring $KERNEL_NEED"

CPU_MODEL="$(lscpu | awk -F: '/Model name/ {gsub(/^[ \t]+/,"",$2); print $2; exit}')"
CPU_NEED="$(read_spec "['cpu']['model_substring']")"
grep -qiF "$CPU_NEED" <<<"$CPU_MODEL" || fail "CPU '$CPU_MODEL' missing '$CPU_NEED'"

[[ "$(nproc --all)" == "$(read_spec "['cpu']['logical_cpus']")" ]] || fail "CPU count mismatch"

g++ --version | head -1 | grep -qF "$(read_spec "['compiler']['version_substring']")" || fail "g++ version mismatch"

LLVM_MAJOR="$(read_spec "['llvm']['major']")"
grep -q "LLVM_DIR:PATH=/usr/lib/llvm-${LLVM_MAJOR}" "$BUILD_DIR/CMakeCache.txt" \
  || fail "build not using LLVM $LLVM_MAJOR"

GOV_PATH="/sys/devices/system/cpu/cpu${PIN_CORE}/cpufreq/scaling_governor"
if [[ -f "$GOV_PATH" ]]; then
  [[ "$(cat "$GOV_PATH")" == "performance" ]] || fail "cpufreq governor not performance"
else
  [[ "$(read_spec "['cpufreq']['policy']")" == "wsl2_exempt" ]] || fail "cpufreq policy"
fi

echo "pinned_host_check=ok"
