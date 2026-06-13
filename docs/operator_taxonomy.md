# Operator Taxonomy

Source inventory: `src/interpreter.cpp`, `src/jit_compiler.cpp`, `src/runtime.cpp`.

| Operator | Class | Window Length Parameter | Rationale |
|---|---|---|---|
| `mid` | Stateless | No | Reads current `bid`/`ask` from `MarketState`; no per-node history. |
| `bid` | Stateless | No | Reads current best bid only. |
| `ask` | Stateless | No | Reads current best ask only. |
| `spread` | Stateless | No | Computes `ask - bid` from current tick only. |
| `ema` | Rolling-window/stateful | Yes (`period`) | Keeps per-node EMA state across ticks; smoothing factor derived from period. |
| `ema_alpha` | Stateful recurrent | No | Keeps per-node EMA state across ticks, but takes the continuous smoothing factor `alpha` directly instead of deriving it from an integer period. |
| `sma` | Rolling-window/stateful | Yes (`period`) | Uses ring buffer stats over trailing window. |
| `rolling_std` | Rolling-window/stateful | Yes (`period`) | Uses rolling Welford state over the trailing window. |
| `rolling_min` | Rolling-window/stateful | Yes (`period`) | Maintains a monotonic deque over the trailing window. |
| `rolling_max` | Rolling-window/stateful | Yes (`period`) | Maintains a monotonic deque over the trailing window. |
| `zscore` | Rolling-window/stateful | Yes (`period`) | Uses rolling mean/std state over the trailing window. |
| `vwap` | Rolling-window/stateful | Yes (`period`) | Maintains rolling price/volume buffers and sums. |
| `lag` | Rolling-window/stateful | Yes (`period`) | Ring buffer indexed by lag period. |
| `rolling_corr` | Rolling-window/stateful | Yes (`period`) | Maintains rolling paired-series sums over a trailing window. |
| `rolling_beta` | Rolling-window/stateful | Yes (`period`) | Reuses the paired-series rolling state and readout formulas. |
| `kalman1d` | Stateful recurrent | No | Tracks posterior estimate and variance across ticks with continuous `q`/`r` parameters. |
| `cross_above` | Stateful (non-windowed) | No | Tracks prior relation via per-node `CrossState`; no configurable window length. |
| `cross_below` | Stateful (non-windowed) | No | Tracks prior relation via per-node `CrossState`; no configurable window length. |

## Counts

- Total built-ins in the core set: **18**
- Stateless: **4**
- Rolling-window/stateful with configurable window length: **10**
- Stateful without window length: **4** (`ema_alpha`, `kalman1d`, `cross_above`, `cross_below`)

## Parameter-Sensitivity Policy

Autodiff follows the conventions in `docs/autodiff_design.md`:

| Operator family | Sensitivity status | Notes |
|---|---|---|
| Arithmetic, `abs`, `log`, `sqrt` | Defined | Checked against central finite differences in `gradient_parity_test`. |
| Market reads (`mid`, `bid`, `ask`, `spread`) | Zero w.r.t. params | They are runtime inputs, not trainable parameters. |
| `ema_alpha` | Defined | Continuous-alpha recurrent sensitivity is carried in persistent per-node state. |
| `sma`, `lag` | Defined | Linear window operators propagate sensitivities through matching ring-buffer state. |
| `rolling_std`, `zscore` | Defined | Uses the same rolling Welford update order as the primal runtime. |
| `rolling_corr`, `rolling_beta` | Defined | Sensitivities propagate through the maintained rolling sums. |
| `kalman1d` | Defined | Sensitivities are defined for continuous `q` and `r`. |
| `rolling_min`, `rolling_max` | Subgradient | Routes sensitivity to the active extremum sample using the runtime tie-breaking rule. |
| `cross_above`, `cross_below` | Stop-gradient | Output gradient is defined as zero. |
| Comparisons, `&&`, `||`, predicate part of `if` | Stop-gradient | The selected branch still propagates gradient piecewise, but the predicate itself has zero sensitivity. |

## Window-Length Notes

- `ema`, `sma`, `rolling_std`, `zscore`, `rolling_min`, `rolling_max`, `vwap`, `lag`, `rolling_corr`, and `rolling_beta` all keep integer window semantics.
- `rolling_std` and `zscore` require `period >= 2`.
- Integer `period` values are part of the DSL surface, not trainable parameters.
