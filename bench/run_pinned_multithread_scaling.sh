#!/usr/bin/env bash
# Pinned-host multi-threaded multi-symbol scaling harness (P2 + homogeneous P/E).
# Usage:
#   bash bench/run_pinned_multithread_scaling.sh [build_dir]
#   bash bench/run_pinned_multithread_scaling.sh [build_dir] --cores 1,2,3,4,5,6,7 \
#     --thread-counts 1,2,4,6 --out-json bench/results/multithread_scaling_pcores.json \
#     --out-md bench/results/multithread_scaling_pcores.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="build-wsl"
CORES=""
THREADS=""
OUT_JSON="$ROOT/bench/results/multithread_scaling.json"
OUT_MD="$ROOT/bench/results/multithread_scaling.md"
LOG="$ROOT/bench/results/multithread_scaling_run.log"
LABEL="hybrid-mixed"

if [[ $# -gt 0 && "$1" != --* ]]; then
  BUILD_DIR="$1"
  shift
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cores)
      CORES="${2:-}"
      shift 2
      ;;
    --thread-counts)
      THREADS="${2:-}"
      shift 2
      ;;
    --out-json)
      OUT_JSON="${2:-}"
      if [[ "$OUT_JSON" != /* ]]; then OUT_JSON="$ROOT/$OUT_JSON"; fi
      shift 2
      ;;
    --out-md)
      OUT_MD="${2:-}"
      if [[ "$OUT_MD" != /* ]]; then OUT_MD="$ROOT/$OUT_MD"; fi
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      LOG="$ROOT/bench/results/multithread_scaling_${LABEL//[^a-zA-Z0-9_-]/_}_run.log"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [build_dir] [--cores LIST] [--thread-counts LIST] [--out-json PATH] [--out-md PATH] [--label NAME]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

SPEC="$ROOT/bench/pinned_host_spec.json"
BENCH="$BUILD_DIR/multithread_scaling_benchmark"

if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: $BENCH not found. Build with: cmake --build $BUILD_DIR -j --target multithread_scaling_benchmark"
  exit 1
fi

bash "$ROOT/bench/pinned_host_check.sh" "$BUILD_DIR"

read_spec() { python3 -c "import json; print(json.load(open('$SPEC'))$1)"; }

SYMBOLS=10000
BENCH_SECONDS=60

if [[ -z "$CORES" ]]; then
  PIN_BASE="$(read_spec "['pin_core']")"
  CORES="$(python3 - <<PY
base = int("$PIN_BASE")
print(",".join(str(base + i) for i in range(16)))
PY
)"
  THREADS="1,2,4,8,16"
  LABEL="hybrid-mixed"
fi

if [[ -z "$THREADS" ]]; then
  THREADS="1,2,4,8,16"
fi

IFS=',' read -r -a CORE_ARR <<< "$CORES"
IFS=',' read -r -a THREAD_ARR <<< "$THREADS"

python3 - "${CORE_ARR[@]}" <<'PY' || { echo "ERROR: --cores must be a comma-separated list of consecutive CPU indices"; exit 1; }
import sys
cores = [int(x) for x in sys.argv[1:]]
if len(cores) < 1:
    raise SystemExit(1)
for i in range(1, len(cores)):
    if cores[i] != cores[i - 1] + 1:
        raise SystemExit(f"non-consecutive core list at {cores[i-1]} -> {cores[i]}")
if 0 in cores:
    raise SystemExit("CPU 0 is reserved for the OS; exclude it from --cores")
PY

PIN_BASE="${CORE_ARR[0]}"
MAX_THREADS="${#CORE_ARR[@]}"

mkdir -p "$ROOT/bench/results"
rm -f "$LOG"
echo "Running multithread scaling label=$LABEL symbols=$SYMBOLS seconds=$BENCH_SECONDS pin_core_base=$PIN_BASE cores=$CORES threads=$THREADS" | tee "$LOG"

for T in "${THREAD_ARR[@]}"; do
  if [[ "$T" -gt "$MAX_THREADS" ]]; then
    echo "skip_threads=$T (exceeds ${MAX_THREADS} cores in --cores)" | tee -a "$LOG"
    continue
  fi
  echo "--- threads=$T cores=${CORES} ---" | tee -a "$LOG"
  taskset -c "${CORES}" "$BENCH" --symbols "$SYMBOLS" --seconds "$BENCH_SECONDS" \
    --pin-core-base "$PIN_BASE" --threads "$T" 2>&1 | tee -a "$LOG"
done

python3 - "$SPEC" "$BUILD_DIR" "$LOG" "$OUT_JSON" "$OUT_MD" "$CORES" "$THREADS" "$LABEL" <<'PY'
import json, re, subprocess, sys
from datetime import datetime, timezone

spec_path, build_dir, log_path, out_json, out_md, cores_csv, threads_csv, label = sys.argv[1:9]
spec = json.load(open(spec_path))
cores = [int(x) for x in cores_csv.split(",") if x]
threads_requested = [int(x) for x in threads_csv.split(",") if x]
pin_base = cores[0] if cores else spec["pin_core"]

rows = []
pat = re.compile(
    r"threads=(\d+) passes=(\d+) duration_sec=([\d.]+) throughput_symbols_per_sec=([\d.eE+-]+) "
    r"scaling_vs_1thread=([\d.eE+-]+) efficiency_pct=([\d.eE+-]+) "
    r"pass_lat_ns_p50=([\d.eE+-]+) pass_lat_ns_p99=([\d.eE+-]+)"
)
for line in open(log_path):
    m = pat.search(line)
    if m:
        rows.append({
            "threads": int(m.group(1)),
            "passes": int(m.group(2)),
            "duration_sec": float(m.group(3)),
            "throughput_symbols_per_sec": float(m.group(4)),
            "scaling_vs_1thread": float(m.group(5)),
            "efficiency_pct": float(m.group(6)),
            "pass_lat_ns_p50": float(m.group(7)),
            "pass_lat_ns_p99": float(m.group(8)),
        })
if not rows:
    raise SystemExit("no benchmark rows parsed from log")

rows.sort(key=lambda r: r["threads"])
baseline = next((r["throughput_symbols_per_sec"] for r in rows if r["threads"] == 1), None)
if baseline is None:
    raise SystemExit("missing threads=1 baseline row")
for r in rows:
    r["scaling_vs_1thread"] = r["throughput_symbols_per_sec"] / baseline
    r["efficiency_pct"] = (r["scaling_vs_1thread"] / r["threads"]) * 100.0

git_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
doc = {
    "canonical_id": spec["canonical_id"],
    "label": label,
    "generated_at_utc": datetime.now(timezone.utc).isoformat(),
    "git_commit": git_sha,
    "build_dir": build_dir,
    "n_symbols": 10000,
    "seconds_per_thread_count": 60,
    "pin_cores": cores,
    "pin_core_base": pin_base,
    "thread_counts_requested": threads_requested,
    "numa": "single node (WSL2); no numactl — documented in bench/PINNED_HOST.md",
    "rows": rows,
}
json.dump(doc, open(out_json, "w"), indent=2)

peak = max(rows, key=lambda r: r["threads"])
core_end = cores[peak["threads"] - 1] if peak["threads"] <= len(cores) else cores[-1]
repro_cores = cores_csv
repro_threads = threads_csv

md = [
    f"# Pinned-host multi-threaded multi-symbol scaling ({label})",
    "",
    f"- Canonical host: `{spec['canonical_id']}` — see `bench/PINNED_HOST.md`",
    f"- Git commit: `{git_sha}`",
    f"- Symbols: 10,000; sustained run: 60s per thread count",
    f"- Pin cores (host-detected): `{cores_csv}` (`taskset -c {cores_csv}`; workers use `--pin-core-base {pin_base}` + thread index)",
    f"- Thread counts: `{threads_csv}`",
    "",
    "## Throughput and scaling",
    "",
    "| Threads | Symbols/sec | Scaling vs 1T | Efficiency | Pass p50 (ns) | Pass p99 (ns) |",
    "|---:|---:|---:|---:|---:|---:|",
]
for r in sorted(rows, key=lambda x: x["threads"]):
    md.append(
        f"| {r['threads']} | {r['throughput_symbols_per_sec']:.0f} | {r['scaling_vs_1thread']:.2f}x | "
        f"{r['efficiency_pct']:.1f}% | {r['pass_lat_ns_p50']:.0f} | {r['pass_lat_ns_p99']:.0f} |"
    )
md += [
    "",
    f"At **{peak['threads']}** threads: **{peak['scaling_vs_1thread']:.2f}x** throughput vs 1 thread, "
    f"**{peak['efficiency_pct']:.1f}%** parallel efficiency (cores `{pin_base}`–`{core_end}`).",
    "",
    "## Reproduce",
    "",
    "```bash",
    f"bash bench/run_pinned_multithread_scaling.sh {build_dir} \\",
    f"  --cores {repro_cores} \\",
    f"  --thread-counts {repro_threads} \\",
    f"  --out-json bench/results/$(basename '{out_json}') \\",
    f"  --out-md bench/results/$(basename '{out_md}') \\",
    f"  --label {label}",
    "```",
    "",
    f"Raw log: `{log_path.replace(chr(92), '/')}`. JSON: `{out_json.replace(chr(92), '/')}`.",
    "",
]
open(out_md, "w").write("\n".join(md))
print(f"Wrote {out_json}")
print(f"Wrote {out_md}")
PY

echo "Pinned multithread scaling artifacts ready ($LABEL)."
