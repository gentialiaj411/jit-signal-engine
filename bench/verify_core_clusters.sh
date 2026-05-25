#!/usr/bin/env bash
# Verify P-cluster (0-7) vs E-cluster (8-23) by pinned group throughput on WSL.
set -euo pipefail
WORK=2.0
bench_group() {
  local cpus="$1"
  python3 - "$cpus" "$WORK" <<'PY'
import os, subprocess, sys, time
cpus = [int(x) for x in sys.argv[1].split(",")]
sec = float(sys.argv[2])
os.sched_setaffinity(0, set(cpus))
deadline = time.perf_counter() + sec
ops = 0
x = 1
while time.perf_counter() < deadline:
    for _ in range(50000):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
    ops += 50000
print(ops / sec / 1e6)
PY
}
p_mops=$(bench_group "0,1,2,3,4,5,6,7")
e_mops=$(bench_group "8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23")
echo "cluster_0_7_Mops_s=$p_mops"
echo "cluster_8_23_Mops_s=$e_mops"
python3 - "$p_mops" "$e_mops" <<'PY'
import sys
p, e = map(float, sys.argv[1:3])
if p <= e:
    raise SystemExit("ERROR: CPUs 0-7 not faster than 8-23; mapping ambiguous")
print("verify=pass P_cluster_faster")
PY
