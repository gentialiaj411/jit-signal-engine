# Phase 0 [CONFIRM] resolutions (pre-benchmark)

## [CONFIRM-1] `bench/results.csv` lowering provenance

**Status: VERIFIED — lowering was OFF**

Evidence:
- `bench/results.csv.meta.json` command: `bash bench/run_benchmarks.sh ./build-wsl ./bench/results.csv 0 1000000` — no `--lower-stateful` flag.
- `bench/run_benchmarks.sh` does not pass `--lower-stateful`.
- `bench/signal_benchmark.cpp` defaults `StatefulLoweringFlags lowering = kAll` unless CLI/env overrides (`--lower-stateful=none`, `JITSE_LOWER_STATEFUL=none`).

Therefore checked-in `results.csv` JIT columns are **JIT with lowering=none** (opaque `jit_rt_*` calls).

Note: meta uses `pin_core: 0`, not the canonical pinned core 2 (`bench/PINNED_HOST.md`).

## [CONFIRM-2] Hand-written `hw_*` baseline fairness

**Status: PARTIALLY UNFAIR — not a per-signal ceiling today**

Evidence from `bench/signal_benchmark.cpp` (~607–658):
- The `hw_*` path is a **single hardcoded spread**: `(mid0 - mid1)` over instruments 0 and 1.
- It runs for every benchmark invocation when `tickers.size() >= 2`, **regardless of which signal is measured**.
- It does **not** implement momentum, zscore, vwap, or fused `eval_all` logic.

Implications:
- For `spread`: hw is a plausible ceiling (same computation).
- For `spread_z`, `z`, `dev`, `<all_signals>`: hw measures raw spread throughput, **not** the signal under test — jit_on÷hw ratios from `results.csv` **overstate** the gap (hw is artificially fast).
- For `momentum`: `hw_*` is `nan` (likely single-instrument program — `tickers.size() < 2`).

**Phase 0 action:** treat current hw as a **spread-only floor**, not a fair fused ceiling. Phase 2+ work should add honest per-signal hardcoded baselines before quoting jit÷hw gaps for non-spread signals.
