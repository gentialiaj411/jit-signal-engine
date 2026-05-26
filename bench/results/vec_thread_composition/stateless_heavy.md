# vec x threads composition benchmark (P12)

Program: `stateless_heavy.sig`

Symbols: 16 | Lanes: 4 | Threads: 4 | Events/symbol: 200000 | Runs/scenario: 5

Total symbol-events per scenario: 3.2e+06

Throughput is reported from the BEST run of 5 (consistent with cross_symbol_benchmark). The median column is a less-cherry-picked second view; the spread column is (slowest-fastest)/fastest as a percentage.

| scenario | best (M sym-evs/s) | median (M sym-evs/s) | spread | speedup vs scalar+1T |
| --- | ---: | ---: | ---: | ---: |
| scalar+1T | 173.38 | 170.65 | 7.25% | 1.00x |
| scalar+4T | 314.27 | 299.84 | 10.40% | 1.81x |
| vec(K=4)+1T | 297.21 | 284.81 | 7.72% | 1.71x |
| vec(K=4)+4T | 752.97 | 647.02 | 21.58% | 4.34x |

## Per-run wall-times (seconds)

* `scalar+1T`: 0.02s, 0.02s, 0.02s, 0.02s, 0.02s
* `scalar+4T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s
* `vec(K=4)+1T`: 0.01s, 0.01s, 0.01s, 0.01s, 0.01s
* `vec(K=4)+4T`: 0.01s, 0.00s, 0.01s, 0.00s, 0.00s

## Interpretation

* `vec+NT vs scalar+1T` is the **composition factor** the roadmap claims. For stateless programs it should be roughly `lanes x threads`. For stateful programs the lane axis is muted (P10 runs stateful ops via per-lane scalarized fan-out, so the per-call cost is ~K serial helper calls per group), so the composition factor collapses to roughly `threads`.
* `vec+1T / scalar+1T` isolates the LANE axis. Numbers below 1.0x for stateful programs are expected and consistent with the P10 docs in `docs/cross_symbol_vectorization_stateful.md`.
* `scalar+NT / scalar+1T` isolates the THREAD axis. This is well-behaved up to the P-core count; once HT kicks in or the memory subsystem saturates, scaling tails off.
