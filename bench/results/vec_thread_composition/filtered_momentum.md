# vec x threads composition benchmark (P12)

Program: `filtered_momentum.sig`

Symbols: 16 | Lanes: 4 | Threads: 4 | Events/symbol: 100000 | Runs/scenario: 5

Total symbol-events per scenario: 1.6e+06

Throughput is reported from the BEST run of 5 (consistent with cross_symbol_benchmark). The median column is a less-cherry-picked second view; the spread column is (slowest-fastest)/fastest as a percentage.

| scenario | best (M sym-evs/s) | median (M sym-evs/s) | spread | speedup vs scalar+1T |
| --- | ---: | ---: | ---: | ---: |
| scalar+1T | 9.80 | 9.37 | 5.51% | 1.00x |
| scalar+4T | 31.25 | 29.95 | 14.33% | 3.19x |
| vec(K=4)+1T | 9.15 | 8.54 | 15.18% | 0.93x |
| vec(K=4)+4T | 30.74 | 29.95 | 7.43% | 3.14x |

## Per-run wall-times (seconds)

* `scalar+1T`: 0.16s, 0.17s, 0.17s, 0.17s, 0.17s
* `scalar+4T`: 0.05s, 0.06s, 0.05s, 0.06s, 0.05s
* `vec(K=4)+1T`: 0.18s, 0.20s, 0.19s, 0.17s, 0.20s
* `vec(K=4)+4T`: 0.05s, 0.05s, 0.05s, 0.06s, 0.05s

## Interpretation

* `vec+NT vs scalar+1T` is the **composition factor** the roadmap claims. For stateless programs it should be roughly `lanes x threads`. For stateful programs the lane axis is muted (P10 runs stateful ops via per-lane scalarized fan-out, so the per-call cost is ~K serial helper calls per group), so the composition factor collapses to roughly `threads`.
* `vec+1T / scalar+1T` isolates the LANE axis. Numbers below 1.0x for stateful programs are expected and consistent with the P10 docs in `docs/cross_symbol_vectorization_stateful.md`.
* `scalar+NT / scalar+1T` isolates the THREAD axis. This is well-behaved up to the P-core count; once HT kicks in or the memory subsystem saturates, scaling tails off.
