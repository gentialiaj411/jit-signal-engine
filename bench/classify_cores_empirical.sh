#!/usr/bin/env bash
# Classify logical CPUs by sustained throughput when pinned.
# WSL2 often reports MAXMHZ as "-" in lscpu; use this script as empirical fallback.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

NCPU=$(nproc)
WORK_SEC=1.0
REPEATS=3
OUT="$ROOT/bench/results/core_classify_raw.txt"
mkdir -p "$ROOT/bench/results"

python3 - "$NCPU" "$WORK_SEC" "$REPEATS" "$OUT" <<'PY'
import os, statistics, subprocess, sys

ncpu = int(sys.argv[1])
work_sec = float(sys.argv[2])
repeats = int(sys.argv[3])
out_path = sys.argv[4]

def bench_cpu(cpu: int) -> float:
    code = r"""
import os, time, sys
cpu = int(sys.argv[1])
sec = float(sys.argv[2])
os.sched_setaffinity(0, {cpu})
deadline = time.perf_counter() + sec
ops = 0
x = 1
while time.perf_counter() < deadline:
    for _ in range(20000):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
    ops += 20000
print(ops / sec)
"""
    samples = []
    for _ in range(repeats):
        r = subprocess.run(
            ["python3", "-c", code, str(cpu), str(work_sec)],
            capture_output=True,
            text=True,
            timeout=work_sec + 10,
        )
        if r.returncode != 0:
            raise RuntimeError(f"cpu{cpu} failed: {r.stderr}")
        samples.append(float(r.stdout.strip()))
    return statistics.median(samples)

rows = []
for cpu in range(ncpu):
    mops = bench_cpu(cpu) / 1e6
    rows.append((cpu, mops))
    print(f"cpu{cpu} median={mops:.3f} Mops/s", flush=True)

rates = sorted([v for _, v in rows])
# Expect two clusters (8 P + 16 E on 275HX). Find split maximizing between-cluster gap.
best_gap = -1.0
best_idx = 7
for i in range(4, min(12, len(rates) - 1)):
    lo = rates[i]
    hi = rates[i + 1]
    gap = hi - lo
    if gap > best_gap:
        best_gap = gap
        best_idx = i
threshold = (rates[best_idx] + rates[best_idx + 1]) / 2.0

p_cores = sorted([c for c, v in rows if v >= threshold])
e_cores = sorted([c for c, v in rows if v < threshold])

with open(out_path, "w") as f:
    f.write(f"work_sec={work_sec} repeats={repeats}\n")
    f.write(f"threshold_Mops_s={threshold} gap={best_gap}\n")
    for c, v in sorted(rows):
        f.write(f"cpu{c} median_Mops_s={v}\n")
    f.write(f"P_CORES={','.join(map(str, p_cores))}\n")
    f.write(f"E_CORES={','.join(map(str, e_cores))}\n")

print(f"THRESHOLD_MOPS={threshold}")
print(f"P_COUNT={len(p_cores)} E_COUNT={len(e_cores)}")
print(f"P_CORES={','.join(map(str, p_cores))}")
print(f"E_CORES={','.join(map(str, e_cores))}")
PY

echo "Wrote $OUT"
