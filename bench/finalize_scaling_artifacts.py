#!/usr/bin/env python3
"""Merge benchmark log lines into scaling JSON/MD (used when runs are split across invocations)."""
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def parse_rows(log_text: str) -> list[dict]:
    pat = re.compile(
        r"threads=(\d+) passes=(\d+) duration_sec=([\d.]+) throughput_symbols_per_sec=([\d.eE+-]+) "
        r"scaling_vs_1thread=([\d.eE+-]+) efficiency_pct=([\d.eE+-]+) "
        r"pass_lat_ns_p50=([\d.eE+-]+) pass_lat_ns_p99=([\d.eE+-]+)"
    )
    rows = []
    for line in log_text.splitlines():
        m = pat.search(line)
        if m:
            rows.append(
                {
                    "threads": int(m.group(1)),
                    "passes": int(m.group(2)),
                    "duration_sec": float(m.group(3)),
                    "throughput_symbols_per_sec": float(m.group(4)),
                    "pass_lat_ns_p50": float(m.group(7)),
                    "pass_lat_ns_p99": float(m.group(8)),
                }
            )
    if not rows:
        raise SystemExit("no benchmark rows parsed")
    rows.sort(key=lambda r: r["threads"])
    baseline = next(r["throughput_symbols_per_sec"] for r in rows if r["threads"] == 1)
    for r in rows:
        r["scaling_vs_1thread"] = r["throughput_symbols_per_sec"] / baseline
        r["efficiency_pct"] = (r["scaling_vs_1thread"] / r["threads"]) * 100.0
    return rows


def write_artifacts(
    *,
    label: str,
    cores_csv: str,
    threads_csv: str,
    log_path: Path,
    out_json: Path,
    out_md: Path,
    repro_cmd: str,
) -> None:
    spec = json.loads((ROOT / "bench/pinned_host_spec.json").read_text())
    cores = [int(x) for x in cores_csv.split(",") if x]
    pin_base = cores[0]
    rows = parse_rows(log_path.read_text())
    git_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True, cwd=ROOT).strip()
    doc = {
        "canonical_id": spec["canonical_id"],
        "label": label,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": git_sha,
        "build_dir": "build-wsl",
        "n_symbols": 10000,
        "seconds_per_thread_count": 60,
        "pin_cores": cores,
        "pin_core_base": pin_base,
        "thread_counts_requested": [int(x) for x in threads_csv.split(",") if x],
        "numa": "single node (WSL2); no numactl — documented in bench/PINNED_HOST.md",
        "rows": rows,
    }
    out_json.write_text(json.dumps(doc, indent=2) + "\n")
    peak = max(rows, key=lambda r: r["threads"])
    core_end = cores[peak["threads"] - 1]
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
    for r in rows:
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
        repro_cmd,
        "```",
        "",
        f"Raw log: `{log_path.relative_to(ROOT).as_posix()}`. JSON: `{out_json.relative_to(ROOT).as_posix()}`.",
        "",
    ]
    out_md.write_text("\n".join(md))
    print(f"Wrote {out_json}")
    print(f"Wrote {out_md}")


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "pcores":
        logs = [
            ROOT / "bench/results/pcores_t1.log",
            ROOT / "bench/results/pcores_t246.log",
        ]
        combined = ROOT / "bench/results/multithread_scaling_pcores_run.log"
        combined.write_text("".join(p.read_text() for p in logs if p.exists()))
        write_artifacts(
            label="pcores-only",
            cores_csv="1,2,3,4,5,6,7",
            threads_csv="1,2,4,6",
            log_path=combined,
            out_json=ROOT / "bench/results/multithread_scaling_pcores.json",
            out_md=ROOT / "bench/results/multithread_scaling_pcores.md",
            repro_cmd=(
                "bash bench/run_pinned_multithread_scaling.sh build-wsl \\\n"
                "  --cores 1,2,3,4,5,6,7 --thread-counts 1,2,4,6 \\\n"
                "  --out-json bench/results/multithread_scaling_pcores.json \\\n"
                "  --out-md bench/results/multithread_scaling_pcores.md --label pcores-only"
            ),
        )
    elif mode == "ecores":
        log_path = ROOT / "bench/results/multithread_scaling_ecores_run.log"
        write_artifacts(
            label="ecores-only",
            cores_csv="8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23",
            threads_csv="1,2,4,8,16",
            log_path=log_path,
            out_json=ROOT / "bench/results/multithread_scaling_ecores.json",
            out_md=ROOT / "bench/results/multithread_scaling_ecores.md",
            repro_cmd=(
                "bash bench/run_pinned_multithread_scaling.sh build-wsl \\\n"
                "  --cores 8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23 --thread-counts 1,2,4,8,16 \\\n"
                "  --out-json bench/results/multithread_scaling_ecores.json \\\n"
                "  --out-md bench/results/multithread_scaling_ecores.md --label ecores-only"
            ),
        )
