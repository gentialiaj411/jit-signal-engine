#!/usr/bin/env bash
# Phase 3: gap table after lowered-base GEP + per-signal hw_reference baselines.
# Usage: bash bench/run_lowering_gap_phase3.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build-wsl}"
SPEC="$ROOT/bench/pinned_host_spec.json"
BENCH="$BUILD_DIR/signal_benchmark"
IR_DIFF="$BUILD_DIR/stateful_lowering_ir_diff"
OUT_DIR="$ROOT/bench/results/lowering_gap_phase3"
TMP_DIR="${TMPDIR:-/tmp}/jitse_lowering_gap_p3_$$"
mkdir -p "$OUT_DIR" "$OUT_DIR/ir" "$TMP_DIR"

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

{
  echo "# Phase 3 host fingerprint"
  echo "- canonical_id: $(read_spec "['canonical_id']")"
  echo "- pin_core: $PIN_CORE"
  echo "- events: $EVENTS"
  echo "- measure_runs: $MEASURE_RUNS"
  echo "- lowering: kAll (default)"
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
    grab("throughput"), grab("jit_throughput"), grab("hw_throughput"),
    grab("jit_mode"), grab("speedup_median"),
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

echo "signal,events,interp_thr,jit_thr,hw_thr,jit_div_hw,speedup_median,jit_mode" > "$OUT_DIR/matrix.csv"

for entry in "${SIGNALS[@]}"; do
  name="${entry%%:*}"
  spec="${entry#*:}"
  log="$TMP_DIR/${name}.log"
  echo "==> $name (kAll default)"
  # shellcheck disable=SC2086
  taskset -c "$PIN_CORE" "$BENCH" --pin-core "$PIN_CORE" --measure-runs "$MEASURE_RUNS" \
    $spec "$EVENTS" > "$log"
  row="$(parse_run "$log")"
  IFS=',' read -r interp jit hw jit_mode speedup <<< "$row"
  python3 - "$name" "$EVENTS" "$interp" "$jit" "$hw" "$speedup" "$jit_mode" "$OUT_DIR/matrix.csv" <<'PY'
import math, sys
name, events, interp, jit, hw, speedup, jit_mode, out_path = sys.argv[1:]
def f(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else float("nan")
    except Exception:
        return float("nan")
interp_v, jit_v, hw_v = f(interp), f(jit), f(hw)
ratio = jit_v / hw_v if math.isfinite(hw_v) and hw_v > 0 else float("nan")
with open(out_path, "a") as out:
    out.write(f"{name},{events},{interp_v},{jit_v},{hw_v},{ratio},{speedup},{jit_mode}\n")
PY
  cp "$log" "$OUT_DIR/${name}.log"
done

# Cross-signal structural CSE IR evidence (filtered_momentum fused program).
if [[ -x "$IR_DIFF" ]]; then
  "$IR_DIFF" ./examples/filtered_momentum.sig > "$OUT_DIR/ir/filtered_momentum_ir.txt" 2>&1 || true
  if grep -q "call.*jit_rt_.*_lowered_base" "$OUT_DIR/ir/filtered_momentum_ir.txt" 2>/dev/null; then
    echo "WARN: lowered_base extern calls still present in IR dump" >> "$OUT_DIR/ir/README.md"
  else
    echo "No jit_rt_*_lowered_base calls in IR (GEP loads from ctx->lowered_bases)." > "$OUT_DIR/ir/README.md"
  fi
fi

python3 - "$OUT_DIR/matrix.csv" "$OUT_DIR/phase3_table.md" "$ROOT/bench/results/lowering_gap_baseline/baseline_table.md" <<'PY'
import csv, math, re, sys
matrix_path, out_path, baseline_path = sys.argv[1], sys.argv[2], sys.argv[3]
rows = list(csv.DictReader(open(matrix_path)))

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

baseline = {}
if baseline_path:
    try:
        text = open(baseline_path).read()
        for line in text.splitlines():
            if not line.startswith("|") or line.startswith("|---"):
                continue
            parts = [p.strip() for p in line.split("|") if p.strip()]
            if len(parts) >= 6 and parts[0] != "signal":
                baseline[parts[0]] = f(parts[5])  # jit_on÷hw from phase0
    except FileNotFoundError:
        pass

lines = [
    "# Lowering gap Phase 3",
    "",
    "Pinned host; `signal_benchmark` with per-signal `hw_reference` baselines,",
    "ctx GEP for lowered-state bases (no `jit_rt_*_lowered_base` calls),",
    "and fused-program cross-signal structural CSE.",
    "",
    "| signal | jit thr | hw thr | jit÷hw | phase0 jit÷hw |",
    "|---|---:|---:|---:|---:|",
]
for r in rows:
    sig = r["signal"]
    jit_v = f(r["jit_thr"])
    hw_v = f(r["hw_thr"])
    ratio = jit_v / hw_v if hw_v > 0 else float("nan")
    p0 = baseline.get(sig, float("nan"))
    lines.append(f"| {sig} | {fmt(jit_v)} | {fmt(hw_v)} | {fmt(ratio)} | {fmt(p0)} |")

lines += ["", "## Reproduce", "", "```bash", "bash bench/run_lowering_gap_phase3.sh build-wsl", "```"]
open(out_path, "w").write("\n".join(lines) + "\n")
print(f"Wrote {out_path}")
PY

echo "Artifacts in $OUT_DIR"
