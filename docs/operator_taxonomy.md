# Operator Taxonomy

Source inventory: `src/interpreter.cpp`, `src/jit_compiler.cpp`, `src/runtime.cpp`.

| Operator | Class | Window Length Parameter | Rationale |
|---|---|---|---|
| `mid` | Stateless | No | Reads current `bid`/`ask` from `MarketState`; no per-node history. |
| `bid` | Stateless | No | Reads current best bid only. |
| `ask` | Stateless | No | Reads current best ask only. |
| `spread` | Stateless | No | Computes `ask - bid` from current tick only. |
| `ema` | Rolling-window/stateful | Yes (`period`) | Keeps per-node EMA state across ticks; smoothing factor derived from period. |
| `sma` | Rolling-window/stateful | Yes (`period`) | Uses ring buffer stats over trailing window. |
| `rolling_std` | Rolling-window/stateful | Yes (`period`) | Uses ring buffer sums/sumsq over trailing window. |
| `rolling_min` | Rolling-window/stateful | Yes (`period`) | Maintains monotonic deque over trailing window. |
| `rolling_max` | Rolling-window/stateful | Yes (`period`) | Maintains monotonic deque over trailing window. |
| `zscore` | Rolling-window/stateful | Yes (`period`) | Uses rolling mean/std state over trailing window. |
| `vwap` | Rolling-window/stateful | Yes (`period`) | Maintains rolling price/volume buffers and sums. |
| `lag` | Rolling-window/stateful | Yes (`period`) | Ring buffer indexed by lag period. |
| `cross_above` | Stateful (non-windowed) | No | Tracks prior relation via per-node `CrossState`; no configurable window length. |
| `cross_below` | Stateful (non-windowed) | No | Tracks prior relation via per-node `CrossState`; no configurable window length. |

## Counts

- Total built-ins in the core set: **14**
- Stateless: **4**
- Rolling-window/stateful with configurable window length: **8**
- Stateful without window length: **2** (`cross_above`, `cross_below`)
