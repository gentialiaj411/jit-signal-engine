# vec x threads composition benchmark (P12)

Program: `momentum_signal.sig`

Symbols: 16 | Lanes: 4 | Threads: 4 | Events/symbol: 100000 | Runs/scenario: 5

Total symbol-events per scenario: 1.6e+06

Throughput is reported from the BEST run of 5 (consistent with cross_symbol_benchmark). The median column is a less-cherry-picked second view; the spread column is (slowest-fastest)/fastest as a percentage.

| scenario | best (M sym-evs/s) | median (M sym-evs/s) | spread | speedup vs scalar+1T |
| --- | ---: | ---: | ---: | ---: |
| scalar+1T | 150.02 | 147.59 | 17.60% | 1.00x |
| scalar+4T | 223.58 | 207.24 | 9.40% | 1.49x |
| vec(K=4)+1T | 142.76 | 136.64 | 12.18% | 0.95x |
| vec(K=4)+4T | 252.75 | 241.60 | 8.35% | 1.68x |

## Per-run wall-times (seconds)

* `scalar+1T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s
* `scalar+4T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s
* `vec(K=4)+1T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s
* `vec(K=4)+4T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s

## Interpretation

* `vec+NT vs scalar+1T` is the **composition factor** the roadmap claims. For stateless programs it should be roughly `lanes x threads`. For stateful programs the lane axis is muted (P10 runs stateful ops via per-lane scalarized fan-out, so the per-call cost is ~K serial helper calls per group), so the composition factor collapses to roughly `threads`.
* `vec+1T / scalar+1T` isolates the LANE axis. Numbers below 1.0x for stateful programs are expected and consistent with the P10 docs in `docs/cross_symbol_vectorization_stateful.md`.
* `scalar+NT / scalar+1T` isolates the THREAD axis. This is well-behaved up to the P-core count; once HT kicks in or the memory subsystem saturates, scaling tails off.
