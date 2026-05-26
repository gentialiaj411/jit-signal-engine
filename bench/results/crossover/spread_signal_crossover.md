# Compile-vs-interpret crossover: `spread_signal`

Source: `examples/spread_signal.sig`  
Methodology: see [`docs/compile_runtime_crossover.md`](../../../docs/compile_runtime_crossover.md). Compile time is best-of-7; per-event time is best-of-5 over 200000 events each (5% warmup excluded, closed-loop, no batching).

## Compile breakdown (best run)

| Phase | ns | % of total |
|---|---:|---:|
| AST -> IR emission | 64007 | 4.8818% |
| LLVM O2 pipeline | 209550 | 15.9823% |
| ORC codegen + lookup | 1023136 | 78.0344% |
| **total (best of 7)** | **1311135** | 100% |
| total (median of 7) | 1349483 | |

## Per-event cost (best run)

| Configuration | ns/event |
|---|---:|
| interpreter | 8.24073 |
| JIT (warm)  | 2.32637 |

## Crossover

`N* = T_compile / (per_event_interp - per_event_jit)`  
`N* = 1311135 / (8.24073 - 2.32637) = **221686 events**`

Equivalently: the JIT pays for itself after about 0.00182686 s of warm event processing (assuming all warm calls go to the JIT).

If you only process **less than 221686** events per session, the interpreter is the better choice (no compile to pay for).

## Plot

![crossover](spread_signal_crossover.svg)
