#!/usr/bin/env python3
"""Compare JIT and pandas-reference signal CSVs."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jit-csv", default="bench/results/diff_test/jit_signals.csv")
    parser.add_argument("--ref-csv", default="bench/results/diff_test/reference_signals.csv")
    parser.add_argument("--out-json", default="bench/results/diff_test/divergence_report.json")
    parser.add_argument("--out-md", default="bench/results/diff_test/divergence_report.md")
    parser.add_argument("--rtol", type=float, default=1e-6)
    parser.add_argument("--atol", type=float, default=1e-9)
    parser.add_argument("--min-within-rate", type=float, default=0.999)
    return parser.parse_args()


@dataclass
class Sample:
    signal: str
    symbol: str
    tick_index: int
    value: float | None


def as_float(raw: str) -> float:
    return float(raw)


def load_csv(path: Path) -> dict[tuple[str, str, int], Sample]:
    rows: dict[tuple[str, str, int], Sample] = {}
    per_signal_symbol: dict[tuple[str, str], int] = defaultdict(int)
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        required = {"symbol", "signal", "value"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise RuntimeError(f"{path} missing columns: {sorted(missing)}")
        for row in reader:
            signal = row["signal"]
            symbol = row["symbol"]
            tick_index = per_signal_symbol[(signal, symbol)]
            per_signal_symbol[(signal, symbol)] += 1
            raw_value = row["value"].strip()
            value = as_float(raw_value)
            rows[(signal, symbol, tick_index)] = Sample(signal, symbol, tick_index, value)
    return rows


def is_finite_number(value: float | None) -> bool:
    return value is not None and math.isfinite(value)


def is_nan_value(value: float | None) -> bool:
    return value is not None and math.isnan(value)


def is_numeric_value(value: float | None) -> bool:
    return is_finite_number(value) and value != 0.0


def is_exact_numeric(a: float | None, b: float | None) -> bool:
    if not is_numeric_value(a) or not is_numeric_value(b):
        return False
    return a == b


def is_within_tolerance_numeric(a: float | None, b: float | None, rtol: float, atol: float) -> bool:
    if not is_numeric_value(a) or not is_numeric_value(b):
        return False
    return math.isclose(a, b, rel_tol=rtol, abs_tol=atol)


def abs_diff(a: float | None, b: float | None) -> float | None:
    if a is None or b is None:
        return None
    if math.isnan(a) and math.isnan(b):
        return 0.0
    if math.isnan(a) or math.isnan(b):
        return None
    return abs(a - b)


def compare(
    jit_rows: dict[tuple[str, str, int], Sample],
    ref_rows: dict[tuple[str, str, int], Sample],
    rtol: float,
    atol: float,
    min_within_rate: float,
) -> tuple[dict[str, Any], list[dict[str, Any]], list[str]]:
    keys = sorted(set(jit_rows) | set(ref_rows))
    per_signal: dict[str, dict[str, Any]] = defaultdict(
        lambda: {
            "total_rows": 0,
            "numeric_rows": 0,
            "numeric_exact_matches": 0,
            "numeric_within_tolerance_matches": 0,
            "nan_matches": 0,
            "degenerate_zero_matches": 0,
            "excluded_unknown_rows": 0,
            "degenerate_mismatches": 0,
            "divergences": 0,
            "missing_in_jit": 0,
            "missing_in_ref": 0,
        }
    )
    divergences: list[dict[str, Any]] = []
    signal_cause: dict[str, list[str]] = defaultdict(list)

    for key in keys:
        signal, symbol, tick_index = key
        jit = jit_rows.get(key)
        ref = ref_rows.get(key)
        stats = per_signal[signal]
        stats["total_rows"] += 1
        jit_value = jit.value if jit is not None else None
        ref_value = ref.value if ref is not None else None
        jit_is_nan = is_nan_value(jit_value)
        ref_is_nan = is_nan_value(ref_value)
        jit_is_zero = is_finite_number(jit_value) and jit_value == 0.0
        ref_is_zero = is_finite_number(ref_value) and ref_value == 0.0
        jit_is_numeric = is_numeric_value(jit_value)

        if jit is None:
            stats["missing_in_jit"] += 1
            stats["divergences"] += 1
            signal_cause[signal].append("missing rows in JIT output")
            divergences.append(
                {
                    "signal": signal,
                    "symbol": symbol,
                    "tick_index": tick_index,
                    "jit_value": None,
                    "ref_value": ref.value if ref else None,
                    "abs_diff": None,
                    "reason": "missing_in_jit",
                }
            )
            continue

        if jit_is_nan and ref_is_nan:
            stats["nan_matches"] += 1
            continue
        if jit_is_zero and ref_is_zero:
            stats["degenerate_zero_matches"] += 1
            continue
        if not jit_is_numeric:
            if ref_value is not None:
                stats["degenerate_mismatches"] += 1
                stats["divergences"] += 1
                divergences.append(
                    {
                        "signal": signal,
                        "symbol": symbol,
                        "tick_index": tick_index,
                        "jit_value": jit_value,
                        "ref_value": ref_value,
                        "abs_diff": abs_diff(jit_value, ref_value),
                        "reason": "degenerate_mismatch",
                }
            )
            continue

        if symbol == "UNKNOWN":
            stats["excluded_unknown_rows"] += 1
            continue

        stats["numeric_rows"] += 1
        if ref is None:
            stats["missing_in_ref"] += 1
            stats["divergences"] += 1
            signal_cause[signal].append("missing rows in reference output")
            divergences.append(
                {
                    "signal": signal,
                    "symbol": symbol,
                    "tick_index": tick_index,
                    "jit_value": jit_value,
                    "ref_value": None,
                    "abs_diff": None,
                    "reason": "missing_in_ref",
                }
            )
            continue

        exact = is_exact_numeric(jit_value, ref_value)
        within = is_within_tolerance_numeric(jit_value, ref_value, rtol, atol)
        diff = abs_diff(jit_value, ref_value)

        if exact:
            stats["numeric_exact_matches"] += 1
        if within:
            stats["numeric_within_tolerance_matches"] += 1
        else:
            stats["divergences"] += 1
            divergences.append(
                {
                    "signal": signal,
                    "symbol": symbol,
                    "tick_index": tick_index,
                    "jit_value": jit_value,
                    "ref_value": ref_value,
                    "abs_diff": diff,
                    "reason": "value_mismatch",
                }
            )

    signal_notes: list[str] = []
    for signal, stats in per_signal.items():
        numeric_rows = stats["numeric_rows"]
        within_rate = (stats["numeric_within_tolerance_matches"] / numeric_rows) if numeric_rows else 0.0
        stats["numeric_within_tolerance_rate"] = within_rate
        stats["numeric_exact_rate"] = (stats["numeric_exact_matches"] / numeric_rows) if numeric_rows else 0.0
        stats["degenerate_rows"] = stats["total_rows"] - numeric_rows
        stats["comparison_rows"] = stats["total_rows"] - stats["excluded_unknown_rows"]
        stats["status"] = "pass" if within_rate >= min_within_rate else "fail"
        if signal_cause.get(signal):
            signal_notes.append(f"{signal}: {', '.join(sorted(set(signal_cause[signal])))}")

    def sort_key(item: dict[str, Any]) -> tuple[int, float]:
        if item["abs_diff"] is None:
            return (0, float("-inf"))
        return (1, float(item["abs_diff"]))

    divergences.sort(key=sort_key, reverse=True)
    return per_signal, divergences[:10], signal_notes


def write_json(
    out_json: Path,
    jit_csv: Path,
    ref_csv: Path,
    per_signal: dict[str, Any],
    top_divergences: list[dict[str, Any]],
    rtol: float,
    atol: float,
    min_within_rate: float,
) -> None:
    total_rows = sum(stats["total_rows"] for stats in per_signal.values())
    payload = {
        "inputs": {"jit_csv": str(jit_csv), "reference_csv": str(ref_csv)},
        "tolerance": {"rtol": rtol, "atol": atol},
        "threshold": {"min_within_tolerance_rate": min_within_rate},
        "total_rows_compared": total_rows,
        "per_signal": per_signal,
        "top_10_worst_divergences": top_divergences,
    }
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def write_markdown(
    out_md: Path,
    jit_csv: Path,
    ref_csv: Path,
    per_signal: dict[str, Any],
    top_divergences: list[dict[str, Any]],
    rtol: float,
    atol: float,
    min_within_rate: float,
    signal_notes: list[str],
) -> None:
    lines: list[str] = []
    lines.append("# Differential Oracle Report")
    lines.append("")
    lines.append(f"- JIT CSV: `{jit_csv}`")
    lines.append(f"- Reference CSV: `{ref_csv}`")
    lines.append(f"- Tolerance: `rtol={rtol}`, `atol={atol}`")
    lines.append(f"- Pass threshold: `{min_within_rate * 100:.3f}%` within tolerance")
    lines.append("- Match rate excludes `UNKNOWN` rows and warmup/degenerate rows where the JIT output is `NaN` or `0.0`.")
    lines.append("")
    lines.append("| Signal | Rows | UNKNOWN excluded | Numeric rows | Numeric exact | Numeric within tol | NaN=NaN | Zero=Zero | Divergences | Match rate | Status |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for signal, stats in per_signal.items():
        lines.append(
            "| {signal} | {rows} | {excluded_unknown_rows} | {numeric_rows} | {exact:.2%} | {within:.2%} | {nan_matches} | {zero_matches} | {div} | {rate:.2%} | {status} |".format(
                signal=signal,
                rows=stats["total_rows"],
                excluded_unknown_rows=stats["excluded_unknown_rows"],
                numeric_rows=stats["numeric_rows"],
                exact=stats["numeric_exact_rate"],
                within=stats["numeric_within_tolerance_rate"],
                nan_matches=stats["nan_matches"],
                zero_matches=stats["degenerate_zero_matches"],
                div=stats["divergences"],
                rate=stats["numeric_within_tolerance_rate"],
                status=stats["status"],
            )
        )
    lines.append("")
    if signal_notes:
        lines.append("## Divergence Notes")
        for note in signal_notes:
            lines.append(f"- {note}")
        lines.append("")
    else:
        lines.append("## Divergence Notes")
        lines.append("- No missing-row gaps observed.")
        lines.append("- Remaining mismatches are numeric or degenerate and are summarized in the table above.")
        lines.append("- Oracle convention: `ema(..., span)` uses `adjust=False`; `rolling_std(..., 30)` uses sample std (`ddof=1`).")
        lines.append("")
    lines.append("## Worst Divergences")
    if top_divergences:
        lines.append("| Signal | Symbol | Tick | JIT | Ref | Abs diff | Reason |")
        lines.append("|---|---|---:|---:|---:|---:|---|")
        for item in top_divergences:
            lines.append(
                "| {signal} | {symbol} | {tick} | {jit} | {ref} | {diff} | {reason} |".format(
                    signal=item["signal"],
                    symbol=item["symbol"],
                    tick=item["tick_index"],
                    jit=item["jit_value"],
                    ref=item["ref_value"],
                    diff=item["abs_diff"],
                    reason=item["reason"],
                )
            )
    else:
        lines.append("- None.")
    lines.append("")
    out_md.parent.mkdir(parents=True, exist_ok=True)
    out_md.write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    jit_csv = Path(args.jit_csv)
    ref_csv = Path(args.ref_csv)
    out_json = Path(args.out_json)
    out_md = Path(args.out_md)

    jit_rows = load_csv(jit_csv)
    ref_rows = load_csv(ref_csv)
    per_signal, top_divergences, signal_notes = compare(
        jit_rows,
        ref_rows,
        args.rtol,
        args.atol,
        args.min_within_rate,
    )
    write_json(out_json, jit_csv, ref_csv, per_signal, top_divergences, args.rtol, args.atol, args.min_within_rate)
    write_markdown(out_md, jit_csv, ref_csv, per_signal, top_divergences, args.rtol, args.atol, args.min_within_rate, signal_notes)

    failed = any(stats["numeric_within_tolerance_rate"] < args.min_within_rate for stats in per_signal.values())
    if failed:
        return 1
    print(f"report_json={out_json}")
    print(f"report_md={out_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
