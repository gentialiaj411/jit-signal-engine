#!/usr/bin/env bash
# Empirical P-core vs E-core detection for hybrid Intel CPUs.
set -euo pipefail

echo "=== lscpu --extended=CPU,CORE,SOCKET,MAXMHZ,MINMHZ ==="
lscpu --extended=CPU,CORE,SOCKET,MAXMHZ,MINMHZ

echo ""
echo "=== per-CPU sysfs (cpufreq max, cpu_capacity, core_type) ==="
for i in $(seq 0 $(($(nproc) - 1))); do
  max_khz=""
  cap=""
  ctype=""
  if [[ -r "/sys/devices/system/cpu/cpu${i}/cpufreq/cpuinfo_max_freq" ]]; then
    max_khz=$(cat "/sys/devices/system/cpu/cpu${i}/cpufreq/cpuinfo_max_freq")
  fi
  if [[ -r "/sys/devices/system/cpu/cpu${i}/cpu_capacity" ]]; then
    cap=$(cat "/sys/devices/system/cpu/cpu${i}/cpu_capacity")
  fi
  if [[ -r "/sys/devices/system/cpu/cpu${i}/topology/core_type" ]]; then
    ctype=$(cat "/sys/devices/system/cpu/cpu${i}/topology/core_type")
  fi
  echo "cpu${i} max_khz=${max_khz:-N/A} capacity=${cap:-N/A} core_type=${ctype:-N/A}"
done
