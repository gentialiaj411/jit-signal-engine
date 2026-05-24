#!/usr/bin/env python3
"""Compute the pandas reference series for the filtered momentum signal."""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd


EXPECTED_SIGNAL_LINES = [
    "signal short_ma = ema(mid(AAPL), 10)",
    "signal long_ma = ema(mid(AAPL), 60)",
    "signal vol = rolling_std(mid(AAPL), 30)",
    "signal raw = short_ma - long_ma",
    "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0",
]


@dataclass
class InstrumentState:
    bid: float = 0.0
    ask: float = 0.0
    last_price: float = 0.0
    volume: float = 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--events-csv",
        default="bench/results/diff_test/journal_events.csv",
        help="CSV exported from the recorded journal",
    )
    parser.add_argument(
        "--signal-file",
        default="examples/filtered_momentum.sig",
        help="Signal file to validate against the oracle",
    )
    parser.add_argument(
        "--out-csv",
        default="bench/results/diff_test/reference_signals.csv",
        help="Output reference signal CSV",
    )
    return parser.parse_args()


def validate_signal_file(path: Path) -> None:
    lines = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    if lines != EXPECTED_SIGNAL_LINES:
        raise RuntimeError(
            "filtered_momentum.sig has changed; update the oracle intentionally instead of reusing a stale formula"
        )


def symbol_key(raw: str) -> str:
    sym = raw.strip()
    return sym if sym else "UNKNOWN"


def replay_events(events_csv: Path) -> list[dict[str, object]]:
    per_symbol: dict[str, InstrumentState] = defaultdict(InstrumentState)
    rows: list[dict[str, object]] = []

    with events_csv.open(newline="") as f:
        reader = csv.DictReader(f)
        required = {"timestamp_ns", "symbol", "event_type", "side", "price", "qty"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise RuntimeError(f"events CSV missing columns: {sorted(missing)}")
        for row in reader:
            sym = symbol_key(row["symbol"])
            state = per_symbol[sym]
            price = int(row["price"])
            qty = int(row["qty"])
            px = float(price) * 1e-4
            if px > 0.0:
                side = row["side"]
                if side == "Buy":
                    state.bid = px
                elif side == "Sell":
                    state.ask = px
                if row["event_type"] in {"Trade", "CrossTrade"}:
                    state.last_price = px
                    if state.bid <= 0.0:
                        state.bid = px
                    if state.ask <= 0.0:
                        state.ask = px
            state.volume = float(qty)

            aapl = per_symbol.get("AAPL", InstrumentState())
            mid = 0.5 * (aapl.bid + aapl.ask)

            rows.append(
                {
                    "timestamp_ns": int(row["timestamp_ns"]),
                    "symbol": sym,
                    "mid": mid,
                }
            )

    return rows


def compute_reference(events_csv: Path, out_csv: Path, signal_file: Path) -> None:
    validate_signal_file(signal_file)
    rows = replay_events(events_csv)
    df = pd.DataFrame(rows)
    if df.empty:
        raise RuntimeError("events CSV is empty")
    df["mid"] = df["mid"].astype("float64")

    grouped = df.groupby("symbol", sort=False)["mid"]
    df["short_ma"] = grouped.transform(lambda s: s.ewm(span=10, adjust=False).mean())
    df["long_ma"] = grouped.transform(lambda s: s.ewm(span=60, adjust=False).mean())
    df["vol"] = grouped.transform(lambda s: s.rolling(30).std(ddof=1))
    df["raw"] = df["short_ma"] - df["long_ma"]
    with np.errstate(divide="ignore", invalid="ignore"):
        df["filtered"] = np.where(
            (df["short_ma"].to_numpy() > df["long_ma"].to_numpy()) & (df["vol"].to_numpy() > 0.0),
            df["raw"].to_numpy() / df["vol"].to_numpy(),
            0.0,
        )
    signal_columns = [
        "short_ma",
        "long_ma",
        "vol",
        "raw",
        "filtered",
    ]

    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ns", "symbol", "signal", "value"])
        for signal_name in signal_columns:
            for row in df.itertuples(index=False):
                value = getattr(row, signal_name)
                if isinstance(value, np.floating):
                    value = float(value)
                writer.writerow([row.timestamp_ns, row.symbol, signal_name, repr(float(value))])


def main() -> int:
    args = parse_args()
    compute_reference(Path(args.events_csv), Path(args.out_csv), Path(args.signal_file))
    print(f"events_csv={args.events_csv}")
    print(f"reference_csv={args.out_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
