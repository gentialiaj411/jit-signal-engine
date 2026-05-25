# Differential Oracle Report

- JIT CSV: `/mnt/c/Users/bhask/Documents/PROJECTS/jit-signal-engine/bench/results/diff_test/jit_signals.csv`
- Reference CSV: `/mnt/c/Users/bhask/Documents/PROJECTS/jit-signal-engine/bench/results/diff_test/reference_signals.csv`
- Tolerance: `rtol=1e-06`, `atol=1e-09`
- Pass threshold: `99.000%` within tolerance
- Match rate excludes `UNKNOWN` rows and warmup/degenerate rows where the JIT output is `NaN` or `0.0`.

| Signal | Rows | UNKNOWN excluded | Numeric rows | Numeric exact | Numeric within tol | NaN=NaN | Zero=Zero | Divergences | Match rate | Status |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| filtered | 1000000 | 4 | 182357 | 0.44% | 99.72% | 0 | 817639 | 512 | 99.72% | pass |
| long_ma | 1000000 | 4 | 779678 | 99.98% | 100.00% | 0 | 220318 | 0 | 100.00% | pass |
| raw | 1000000 | 4 | 779676 | 97.47% | 100.00% | 0 | 220320 | 0 | 100.00% | pass |
| short_ma | 1000000 | 4 | 779678 | 97.49% | 100.00% | 0 | 220318 | 0 | 100.00% | pass |
| vol | 1000000 | 4 | 315661 | 0.23% | 99.74% | 18854 | 665481 | 812 | 99.74% | pass |

## Divergence Notes
- No missing-row gaps observed.
- Remaining mismatches are numeric or degenerate and are summarized in the table above.
- Oracle convention: `ema(..., span)` uses `adjust=False`; `rolling_std(..., 30)` uses sample std (`ddof=1`).

## Worst Divergences
| Signal | Symbol | Tick | JIT | Ref | Abs diff | Reason |
|---|---|---:|---:|---:|---:|---|
| filtered | ABB | 116 | 5243.735922545428 | 5243.721362550752 | 0.014559994676346832 | value_mismatch |
| filtered | HSBC | 3052 | 3272.9553308391673 | 3272.9686885892474 | 0.01335775008010387 | value_mismatch |
| filtered | GOLD | 152 | 2987.129283611971 | 2987.116275611448 | 0.01300800052285922 | value_mismatch |
| filtered | ABB | 120 | 4588.391176400705 | 4588.378436065936 | 0.012740334769659967 | value_mismatch |
| filtered | IVV | 103 | 6509.1858957426375 | 6509.194435279696 | 0.008539537058823043 | value_mismatch |
| filtered | ERIC | 113 | 5819.439991364374 | 5819.4335782304925 | 0.006413133881324029 | value_mismatch |
| filtered | BBL | 10447 | 2322.223005477659 | 2322.217208826434 | 0.005796651224954985 | value_mismatch |
| filtered | ABB | 115 | 3901.4008175138424 | 3901.3952076366554 | 0.005609877186998347 | value_mismatch |
| filtered | RACE | 145 | 1621.7718143582583 | 1621.76678144683 | 0.005032911428315856 | value_mismatch |
| filtered | ABB | 121 | 3193.181159443595 | 3193.1765679250666 | 0.004591518528286542 | value_mismatch |
