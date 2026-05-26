# P12: vec x threads composition benchmark

This folder is the artifact for P12 on the roadmap: a single Markdown
table that pins how the two scaling axes the engine ships -- cross-
symbol vectorization (P2 stateless, P10 stateful) and thread-level
parallelism (P5) -- compose on real programs.

Each row is the throughput of one (lane-count, thread-count) cell of
the 2x2 grid, measured by `vec_thread_composition_benchmark` (in
`bench/vec_thread_composition_benchmark.cpp`). The total work per row
is identical: `symbols * events_per_symbol` market events, with the
same per-symbol event stream replayed in every cell. Numbers are the
best of 5 runs (consistent with `cross_symbol_benchmark`); the per-
program `.md` files also include the median and the run-to-run spread.

## How to read these

Three diagnostic ratios fall out of every row:

* **lane axis only**: `vec+1T / scalar+1T`. What lane-level vectorization
  buys when you can't add threads. P2 (stateless) widens directly to
  `<K x double>`; P10 (stateful) emits per-lane scalarized fan-out and
  therefore charges roughly K serial calls per group. Sub-`K x` numbers
  for stateful programs are expected and documented in
  `docs/cross_symbol_vectorization_stateful.md`.
* **thread axis only**: `scalar+NT / scalar+1T`. What threading buys at
  one lane. Up to the P-core count this is well-behaved; once HT kicks
  in or memory saturates, scaling tails off.
* **composition**: `vec+NT / scalar+1T`. The product. For stateless
  programs this is roughly `lanes x threads`; for stateful programs it
  collapses to roughly `threads` because the lane axis is muted.

This is the actual evidence behind the roadmap claim of "K x N effective
on stateless, N x effective on stateful".

## Captured artifacts

The bench was run on this host (Windows 10 host, WSL2 Ubuntu, 6-core
Intel-class CPU, 4 P-cores used for pinning) with `--symbols=16
--threads=4 --lanes=4`. K=4 matches the canonical AVX2 cell used by P2
and P10. T=4 matches the P-core count on a typical client CPU.

* [`stateless_heavy.md`](stateless_heavy.md) -- the pure-arithmetic
  `stateless_heavy.sig` from `examples/`. P2/P10 widening is pure win:
  composition 4.34x.
* [`momentum_signal.sig` -> `momentum_signal.md`](momentum_signal.md) --
  single-signal stateful program (one rolling op). Small kernel; the
  threading overhead and the stateful fan-out cost both dominate, so
  composition lands at 1.68x. Lane axis is essentially neutral (0.95x).
* [`filtered_momentum.sig` -> `filtered_momentum.md`](filtered_momentum.md)
  -- the conditional + stateful program that motivated P11. After P11
  it runs cleanly under both scalar and vec; threading dominates
  (3.19x), lane axis is 0.93x, composition 3.14x.

## Headline numbers

| program | lane-only | thread-only | composition |
| --- | ---: | ---: | ---: |
| `stateless_heavy.sig` (pure arith) | 1.71x | 1.81x | **4.34x** |
| `momentum_signal.sig` (single signal, stateful) | 0.95x | 1.49x | **1.68x** |
| `filtered_momentum.sig` (multi-signal, stateful, conditional) | 0.93x | 3.19x | **3.14x** |

The honest summary: cross-symbol vectorization is a real win on
stateless workloads where the JIT can widen the whole IR to `<K x
double>`. On stateful workloads it's a correctness win (P10 lets the
program run in vec mode at all) but a near-neutral performance change
(per-lane scalarized fan-out costs ~K serial helper calls per group).
Thread-level parallelism stays roughly orthogonal to the lane axis on
both kinds of programs.

## Reproduce

From the build directory:

```
./vec_thread_composition_benchmark ../examples/stateless_heavy.sig \
  --events=200000 --symbols=16 --threads=4 --lanes=4 --runs=5 \
  --md=../bench/results/vec_thread_composition/stateless_heavy.md
```

Adjust `--symbols`, `--threads`, `--lanes`, and `--events` to suit the
host. The bench enforces that `symbols` is divisible by `lanes` and by
`threads`, and that `symbols / threads` is itself divisible by `lanes`
(so each thread in the vec+NT cell owns a whole number of K-groups).
