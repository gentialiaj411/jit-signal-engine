import csv
import math
import os
import sys

import matplotlib.pyplot as plt


def load_rows(path):
    rows = []
    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)
    return rows


def maybe_float(s):
    try:
        v = float(s)
        return v
    except Exception:
        return math.nan


def main():
    if len(sys.argv) < 2:
        print("Usage: python bench/plot_results.py <results_csv> [out_dir]")
        return 1
    csv_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) >= 3 else "bench"
    os.makedirs(out_dir, exist_ok=True)

    rows = load_rows(csv_path)
    if not rows:
        print("No rows in CSV.")
        return 1

    signals = [r["signal"] for r in rows]
    th = [maybe_float(r["throughput"]) for r in rows]
    p99 = [maybe_float(r["lat_ns_p99"]) for r in rows]
    hw_th = [maybe_float(r.get("hw_throughput", "nan")) for r in rows]

    plt.figure(figsize=(8, 4))
    plt.bar(signals, th, label="Interpreter")
    plt.ylabel("Evaluations / second")
    plt.title("Throughput by Signal")
    plt.xticks(rotation=20)
    plt.tight_layout()
    out1 = os.path.join(out_dir, "throughput.png")
    plt.savefig(out1, dpi=150)
    plt.close()

    plt.figure(figsize=(8, 4))
    plt.bar(signals, p99)
    plt.ylabel("ns")
    plt.title("p99 Latency by Signal")
    plt.xticks(rotation=20)
    plt.tight_layout()
    out2 = os.path.join(out_dir, "latency_p99.png")
    plt.savefig(out2, dpi=150)
    plt.close()

    valid_hw = [not math.isnan(v) for v in hw_th]
    if any(valid_hw):
        width = 0.35
        x = list(range(len(signals)))
        plt.figure(figsize=(8, 4))
        plt.bar([i - width / 2 for i in x], th, width=width, label="Interpreter")
        plt.bar([i + width / 2 for i in x], hw_th, width=width, label="Handwritten C++")
        plt.xticks(x, signals, rotation=20)
        plt.ylabel("Evaluations / second")
        plt.title("Interpreter vs Handwritten Throughput")
        plt.legend()
        plt.tight_layout()
        out3 = os.path.join(out_dir, "throughput_compare.png")
        plt.savefig(out3, dpi=150)
        plt.close()

    print(f"Wrote {out1}")
    print(f"Wrote {out2}")
    if any(valid_hw):
        print(f"Wrote {os.path.join(out_dir, 'throughput_compare.png')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

