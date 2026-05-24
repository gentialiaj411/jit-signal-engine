# Integration with market-data-handler (mdh)

## Boundary
- Source project: `market-data-handler` produces canonical `mf::core::BookEvent` journals.
- Consumer project: `jit-signal-engine` backtest runner reads that journal and converts events into per-symbol market state updates.
- Output: `jit-signal-engine` writes signal time series (`signals.csv`) and IC metrics (`ic_report.json`).

## Data flow
1. `mdh` writes canonical journal records (`JournalHeader` + `JournalRecord` + `BookEvent`).
2. `jit-signal-engine/examples/backtest_runner.cpp` uses `mf::journal::JournalReader` to stream events in journal order.
3. Each event updates symbol state (`bid/ask/last_price/volume`) and triggers one compiled signal-program evaluation for that symbol.
4. Runner emits:
   - `bench/results/backtest/<run_id>/signals.csv`
   - `bench/results/backtest/<run_id>/ic_report.json`

## Current implementation status
- Integration is active in the Phase 4 path via direct mdh journal consumption.
- Build wiring uses CMake cache var `JITSE_MDH_ROOT` to locate mdh headers/sources for `backtest_runner` and determinism test.
- This is WSL/Linux-oriented because mdh journal reader path is Linux-backed.

## Evidence anchor
- Example recorded source:
  - `/mnt/c/Users/bhask/Documents/PROJECTS/market-data-handler/bench/results/itch_1m_ab_source.journal`
- Example integrated output:
  - `bench/results/backtest/phase4_mdh_20260523/ic_report.json`
