#!/usr/bin/env bash
# Pinned-host JIT vs interpreter speedup harness (P0).
# Usage: bash bench/run_pinned_speedup.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build-wsl}"
SPEC="$ROOT/bench/pinned_host_spec.json"
BENCH="$BUILD_DIR/signal_benchmark"
OUT_JSON="$ROOT/bench/results/pinned_host_speedup.json"
OUT_MD="$ROOT/bench/results/pinned_host_speedup.md"
TMP_DIR="${TMPDIR:-/tmp}/jitse_pinned_$$"
mkdir -p "$TMP_DIR" "$ROOT/bench/results"

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

if [[ ! -f "$SPEC" ]]; then
  echo "ERROR: missing $SPEC"
  exit 1
fi
if [[ ! -x "$BENCH" ]]; then
  echo "ERROR: benchmark binary not found: $BENCH"
  echo "Build: cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j"
  exit 1
fi

read_spec() {
  python3 -c "import json; print(json.load(open('$SPEC'))$1)"
}

PIN_CORE="$(read_spec "['pin_core']")"
EVENTS="$(read_spec "['events']")"
MEASURE_RUNS="$(read_spec "['measure_runs']")"

fail() { echo "PINNED HOST CHECK FAILED: $*" >&2; exit 1; }

KERNEL="$(uname -r)"
KERNEL_NEED="$(read_spec "['os']['kernel_substring']")"
if ! grep -qF "$KERNEL_NEED" <<<"$KERNEL"; then
  fail "kernel $KERNEL missing substring $KERNEL_NEED"
fi

CPU_MODEL="$(lscpu | awk -F: '/Model name/ {gsub(/^[ \t]+/,"",$2); print $2; exit}')"
CPU_NEED="$(read_spec "['cpu']['model_substring']")"
if ! grep -qiF "$CPU_NEED" <<<"$CPU_MODEL"; then
  fail "CPU '$CPU_MODEL' missing '$CPU_NEED'"
fi

CPU_COUNT="$(nproc --all)"
CPU_WANT="$(read_spec "['cpu']['logical_cpus']")"
[[ "$CPU_COUNT" == "$CPU_WANT" ]] || fail "CPU count $CPU_COUNT != $CPU_WANT"

g++ --version | head -1 | grep -qF "$(read_spec "['compiler']['version_substring']")" \
  || fail "g++ version mismatch"

LLVM_MAJOR="$(read_spec "['llvm']['major']")"
grep -q "LLVM_DIR:PATH=/usr/lib/llvm-${LLVM_MAJOR}" "$BUILD_DIR/CMakeCache.txt" \
  || fail "build not using LLVM $LLVM_MAJOR"

GOV_PATH="/sys/devices/system/cpu/cpu${PIN_CORE}/cpufreq/scaling_governor"
if [[ -f "$GOV_PATH" ]]; then
  GOV="$(cat "$GOV_PATH")"
  [[ "$GOV" == "performance" ]] || fail "governor is '$GOV' (need performance)"
else
  [[ "$(read_spec "['cpufreq']['policy']")" == "wsl2_exempt" ]] \
    || fail "cpufreq missing and policy is not wsl2_exempt"
  echo "NOTE: WSL2 cpufreq unavailable; use Windows Ultimate Performance on host."
fi

# HT sibling busy check (skip when sibling set is only the pinned core).
SIBLINGS="$(cat "/sys/devices/system/cpu/cpu${PIN_CORE}/topology/thread_siblings_list")"
IFS=',' read -ra SIB_PARTS <<<"$SIBLINGS"
for part in "${SIB_PARTS[@]}"; do
  if [[ "$part" == *-* ]]; then
  for ((c=${part%-*}; c<=${part#*-}; c++)); do
    if [[ "$c" != "$PIN_CORE" ]]; then
      line1="$(grep -E "^cpu${c} " /proc/stat || true)"
      sleep 0.2
      line2="$(grep -E "^cpu${c} " /proc/stat || true)"
      if [[ -n "$line1" && -n "$line2" ]]; then
        read -r _ u1 n1 s1 i1 iw1 irq1 si1 st1 _ <<<"$line1"
        read -r _ u2 n2 s2 i2 iw2 irq2 si2 st2 _ <<<"$line2"
        idle1=$((i1 + iw1)); idle2=$((i2 + iw2))
        total1=$((u1+n1+s1+idle1+irq1+si1+st1)); total2=$((u2+n2+s2+idle2+irq2+si2+st2))
        dt=$((total2-total1)); didle=$((idle2-idle1))
        if [[ "$dt" -gt 0 ]]; then
          busy=$(( (100 * (dt - didle)) / dt ))
          [[ "$busy" -le 15 ]] || fail "HT sibling cpu$c busy ${busy}%"
        fi
      fi
    fi
  done
  else
    if [[ "$part" != "$PIN_CORE" ]]; then
      line1="$(grep -E "^cpu${part} " /proc/stat || true)"
      sleep 0.2
      line2="$(grep -E "^cpu${part} " /proc/stat || true)"
      if [[ -n "$line1" && -n "$line2" ]]; then
        read -r _ u1 n1 s1 i1 iw1 irq1 si1 st1 _ <<<"$line1"
        read -r _ u2 n2 s2 i2 iw2 irq2 si2 st2 _ <<<"$line2"
        idle1=$((i1 + iw1)); idle2=$((i2 + iw2))
        total1=$((u1+n1+s1+idle1+irq1+si1+st1)); total2=$((u2+n2+s2+idle2+irq2+si2+st2))
        dt=$((total2-total1)); didle=$((idle2-idle1))
        if [[ "$dt" -gt 0 ]]; then
          busy=$(( (100 * (dt - didle)) / dt ))
          [[ "$busy" -le 15 ]] || fail "HT sibling cpu$part busy ${busy}%"
        fi
      fi
    fi
  fi
done
echo "Host checks passed (cpu$PIN_CORE siblings: $SIBLINGS)"

run_case() {
  local label="$1"
  shift
  local log="$TMP_DIR/${label}.log"
  echo "" >&2
  echo "=== $label ===" >&2
  taskset -c "$PIN_CORE" "$BENCH" --pin-core "$PIN_CORE" --measure-runs "$MEASURE_RUNS" "$@" 2>&1 | tee "$log" >&2
  grep -q '^jit_mode=enabled$' "$log" || fail "$label: jit_mode not enabled"
  printf '%s\n' "$log"
}

MOM_LOG="$(run_case momentum ./examples/momentum_signal.sig "$EVENTS" "" momentum)"
ALL_LOG="$(run_case all_signals_fused --all-signals ./examples/filtered_momentum.sig "$EVENTS")"

python3 - "$SPEC" "$BUILD_DIR" "$OUT_JSON" "$OUT_MD" "$MOM_LOG" "$ALL_LOG" <<'PY'
import json, random, re, statistics, subprocess, sys
from datetime import datetime, timezone

spec_path, build_dir, out_json, out_md, mom_log, all_log = sys.argv[1:7]
spec = json.load(open(spec_path))

def parse_log(path):
    text = open(path).read()
    interp = [float(m.group(2)) for m in re.finditer(r'measure_interp_run=(\d+) throughput=(\S+)', text)]
    jit = [float(m.group(2)) for m in re.finditer(r'measure_jit_run=(\d+) throughput=(\S+)', text)]
    if not interp:
        m = re.search(r'^throughput=(\S+)', text, re.M)
        if m:
            interp = [float(m.group(1))]
    if not jit:
        m = re.search(r'^jit_throughput=(\S+)', text, re.M)
        if m:
            jit = [float(m.group(1))]
    speedups = [j / i for i, j in zip(interp, jit) if i > 0]
    jit_p99 = [float(m.group(1)) for m in re.finditer(r'measure_jit_run=\d+ throughput=\S+ lat_ns_p99=(\S+)', text)]
    return {
        'interp_throughput_runs': interp,
        'jit_throughput_runs': jit,
        'speedup_runs': speedups,
        'jit_lat_ns_p99_runs': jit_p99,
    }

def bootstrap_ci(values, n_boot=10000, alpha=0.05):
    if not values:
        return float('nan'), float('nan'), float('nan')
    if len(values) == 1:
        return values[0], values[0], values[0]
    meds = []
    for _ in range(n_boot):
        sample = random.choices(values, k=len(values))
        meds.append(statistics.median(sample))
    meds.sort()
    lo = meds[int((alpha / 2) * n_boot)]
    hi = meds[int((1 - alpha / 2) * n_boot) - 1]
    return statistics.median(values), lo, hi

def pct(values, p):
    if not values:
        return float('nan')
    s = sorted(values)
    return s[int(p * (len(s) - 1))]

git_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
kernel = subprocess.check_output(['uname', '-r'], text=True).strip()
cpu = subprocess.check_output(
    "lscpu | awk -F: '/Model name/ {gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'",
    shell=True, text=True).strip()

cases = {}
for key, path in [('momentum', mom_log), ('all_signals_fused', all_log)]:
    raw = parse_log(path)
    speedups = raw['speedup_runs']
    med, lo, hi = bootstrap_ci(speedups)
    cases[key] = {
        **raw,
        'measure_runs': len(speedups),
        'speedup_median': med,
        'speedup_p99': pct(speedups, 0.99),
        'speedup_bootstrap_95pct_ci': [lo, hi],
        'jit_lat_ns_p99_median': statistics.median(raw['jit_lat_ns_p99_runs']) if raw['jit_lat_ns_p99_runs'] else None,
        'jit_lat_ns_p99_p99': pct(raw['jit_lat_ns_p99_runs'], 0.99) if raw['jit_lat_ns_p99_runs'] else None,
    }

doc = {
    'canonical_id': spec['canonical_id'],
    'generated_at_utc': datetime.now(timezone.utc).isoformat(),
    'git_commit': git_sha,
    'host': {'kernel': kernel, 'cpu_model': cpu, 'pin_core': spec['pin_core']},
    'build_dir': build_dir,
    'events': spec['events'],
    'measure_runs': spec['measure_runs'],
    'cases': cases,
}
json.dump(doc, open(out_json, 'w'), indent=2)

lines = [
    '# Pinned-host JIT vs interpreter speedup',
    '',
    f"- Canonical host: `{spec['canonical_id']}` — see `bench/PINNED_HOST.md`",
    f"- Git commit: `{git_sha}`",
    f"- Generated (UTC): {doc['generated_at_utc']}",
    f"- Pin core: {spec['pin_core']}; events/run: {spec['events']:,}; repeats: {spec['measure_runs']}",
    '',
    '## Headline speedups (JIT throughput / interpreter throughput)',
    '',
    '| Case | Median | p99 of runs | 95% bootstrap CI (median) |',
    '|---|---:|---:|---|',
]
labels = {'momentum': 'Momentum (single signal)', 'all_signals_fused': 'All-signals fused'}
for key in ('momentum', 'all_signals_fused'):
    c = cases[key]
    lo, hi = c['speedup_bootstrap_95pct_ci']
    lines.append(f"| {labels[key]} | {c['speedup_median']:.2f}x | {c['speedup_p99']:.2f}x | [{lo:.2f}x, {hi:.2f}x] |")
lines += [
    '',
    '## Reproduce',
    '',
    '```bash',
    'bash bench/run_pinned_speedup.sh build-wsl',
    '```',
    '',
    'Raw data: `bench/results/pinned_host_speedup.json`.',
    '',
]
open(out_md, 'w').write('\n'.join(lines))
print(f'Wrote {out_json}')
print(f'Wrote {out_md}')
PY

echo "Pinned speedup artifacts ready."
