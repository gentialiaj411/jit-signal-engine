#!/usr/bin/env python3
"""Generate a deterministic AAPL event fixture for the differential oracle gate."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-csv", required=True)
    parser.add_argument("--rows", type=int, default=240)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    out_csv = Path(args.out_csv)
    out_csv.parent.mkdir(parents=True, exist_ok=True)

    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "event_index",
                "sequence",
                "timestamp_ns",
                "exchange_ts_ns",
                "ingest_ts_ns",
                "symbol",
                "event_type",
                "side",
                "price",
                "qty",
            ]
        )
        for i in range(args.rows):
            side = "Buy" if i % 2 == 0 else "Sell"
            trend = i * 8
            wave = ((i * 17) % 23) - 11
            spread = 120
            bid = 1_000_000 + trend + wave
            ask = bid + spread
            price = bid if side == "Buy" else ask
            event_type = "Trade" if i % 11 == 0 else "Add"
            timestamp = 1_000_000_000 + i * 1_000
            writer.writerow([i, i + 1, timestamp, timestamp, timestamp, "AAPL", event_type, side, price, 100 + (i % 7)])

    print(f"events_csv={out_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
