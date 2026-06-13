#!/usr/bin/env bash
# Phase 0: four-path lowering gap baseline (pinned host).
# Usage: bash bench/run_lowering_gap_baseline.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build-wsl}"
SPEC="$ROOT/bench/pinned_host_spec.json"
BENCH="$BUILD_DIR/signal_benchmark"
OUT_DIR="$ROOT/bench/results/lowering_gap_baseline"
TMP_DIR="${TMPDIR:-/tmp}/jitse_lowering_gap_$$"
mkdir -p "$OUT_DIR" "$TMP_DIR"

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found: $BENCH"
  exit 1
fi

read_spec() {
  python3 -c "import json; print(json.load(open('$SPEC'))$1)"
}

PIN_CORE="$(read_spec "['pin_core']")"
EVENTS="$(read_spec "['events']")"
MEASURE_RUNS="$(read_spec "['measure_runs']")"

# Host fingerprint for artifact provenance.
{
  echo "# Host fingerprint"
  echo "- canonical_id: $(read_spec "['canonical_id']")"
  echo "- kernel: $(uname -r)"
  echo "- cpu: $(lscpu | awk -F: '/Model name/ {gsub(/^[ \t]+/,"",$2); print $2; exit}')"
  echo "- nproc: $(nproc --all)"
  echo "- g++: $(g++ --version | head -1)"
  echo "- pin_core: $PIN_CORE"
  echo "- events: $EVENTS"
  echo "- measure_runs: $MEASURE_RUNS"
  echo "- build_dir: $BUILD_DIR"
  echo "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT_DIR/host_fingerprint.md"

parse_run() {
  local log="$1"
  python3 - "$log" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
def grab(key):
    m = re.search(rf'^{key}=(.+)$', text, re.M)
    return m.group(1).strip() if m else ""
print(",".join([
    grab("throughput"), grab("lat_ns_p50"), grab("lat_ns_p99"), grab("lat_ns_p999"),
    grab("jit_throughput"), grab("jit_lat_ns_p50"), grab("jit_lat_ns_p99"), grab("jit_lat_ns_p999"),
    grab("hw_throughput"), grab("hw_lat_ns_p50"), grab("hw_lat_ns_p99"), grab("hw_lat_ns_p999"),
    grab("speedup_median"), grab("jit_mode"),
]))
PY
}

SIGNALS=(
  "spread:./examples/spread_signal.sig"
  "momentum:./examples/momentum_signal.sig"
  "spread_z:./examples/zscore_signal.sig"
  "z:./examples/zscore_builtin_signal.sig"
  "dev:./examples/vwap_signal.sig"
  "all_signals:--all-signals ./examples/filtered_momentum.sig"
)

echo "signal,path,events,interp_throughput,interp_p50,interp_p99,interp_p999,jit_throughput,jit_p50,jit_p99,jit_p999,hw_throughput,hw_p50,hw_p99,hw_p999,speedup_median,jit_mode" \
  > "$OUT_DIR/matrix.csv"

for entry in "${SIGNALS[@]}"; do
  name="${entry%%:*}"
  spec="${entry#*:}"
  for lowering in none all; do
    log="$TMP_DIR/${name}_${lowering}.log"
    echo "==> $name lowering=$lowering"
    # shellcheck disable=SC2086
    taskset -c "$PIN_CORE" "$BENCH" --pin-core "$PIN_CORE" --measure-runs "$MEASURE_RUNS" \
      --lower-stateful="$lowering" $spec "$EVENTS" > "$log"
    row="$(parse_run "$log")"
    IFS=',' read -r interp_th interp_p50 interp_p99 interp_p999 jit_th jit_p50 jit_p99 jit_p999 hw_th hw_p50 hw_p99 hw_p999 speedup jit_mode <<< "$row"
    echo "$name,jit_${lowering},$EVENTS,$interp_th,$interp_p50,$interp_p99,$interp_p999,$jit_th,$jit_p50,$jit_p99,$jit_p999,$hw_th,$hw_p50,$hw_p99,$hw_p999,$speedup,$jit_mode" \
      >> "$OUT_DIR/matrix.csv"
    cp "$log" "$OUT_DIR/${name}_jit_${lowering}.log"
  done
done

python3 - "$OUT_DIR/matrix.csv" "$OUT_DIR/baseline_table.md" <<'PY'
import csv, math, sys
matrix_path, out_path = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(matrix_path)))
by_signal = {}
for r in rows:
    by_signal.setdefault(r["signal"], {})[r["path"]] = r

lines = [
    "# Lowering gap baseline (Phase 0)",
    "",
    "Pinned host; `signal_benchmark --measure-runs 30 --pin-core 2`.",
    "Interpreter + hw columns are identical across `jit_none` / `jit_all` runs (same binary invocation aside from lowering flag).",
    "",
    "| signal | interp thr | jit_off thr | jit_on thr | hw thr | jit_on÷hw | jit_off÷interp | jit_on÷interp |",
    "|---|---:|---:|---:|---:|---:|---:|---:|",
]

def f(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else float("nan")
    except Exception:
        return float("nan")

def fmt(v, prec=2):
    if not math.isfinite(v):
        return "nan"
    if v >= 1e6:
        return f"{v/1e6:.2f}M"
    return f"{v:.{prec}f}"

for sig in ["spread", "momentum", "spread_z", "z", "dev", "all_signals"]:
    off = by_signal.get(sig, {}).get("jit_none")
    on = by_signal.get(sig, {}).get("jit_all")
    if not off or not on:
        continue
    interp = f(off["interp_throughput"])
    jit_off = f(off["jit_throughput"])
    jit_on = f(on["jit_throughput"])
    hw = f(off["hw_throughput"])
    ratio_hw = jit_on / hw if math.isfinite(hw) and hw > 0 else float("nan")
    ratio_off = jit_off / interp if interp > 0 else float("nan")
    ratio_on = jit_on / interp if interp > 0 else float("nan")
    lines.append(
        f"| {sig} | {fmt(interp)} | {fmt(jit_off)} | {fmt(jit_on)} | {fmt(hw)} | {fmt(ratio_hw)} | {fmt(ratio_off)} | {fmt(ratio_on)} |"
    )

lines += [
    "",
    "## Reproduce",
    "",
    "```bash",
    "bash bench/run_lowering_gap_baseline.sh build-wsl",
    "```",
]
open(out_path, "w").write("\n".join(lines) + "\n")
print(f"Wrote {out_path}")
PY

echo "Artifacts in $OUT_DIR"
