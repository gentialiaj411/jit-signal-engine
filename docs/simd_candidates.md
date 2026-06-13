# SIMD and Vectorization

## Per-operator AVX2 (`bench/results_simd.csv`)

Historical benchmark: explicit LLVM AVX2 lowering for `sma` with runtime feature detection and scalar fallback (`JITSE_FORCE_DISABLE_AVX2`). On the host used for `results_simd.csv`, AVX2 did not beat scalar JIT on `sma(64)` / `sma(128)`; `zscore(128)` was effectively tied.

**Do not** use `results_simd.csv` alone to claim “SIMD wins.” It documents per-operator rolling-window lowering, not cross-symbol throughput.

## Cross-symbol vectorization (canonical speedup story)

**Harness:** `bench/cross_symbol_benchmark.cpp`  
**Programs:**

| File | Character | K=4 speedup (vec vs scalar JIT, pinned) |
|---|---|---|
| `examples/stateless_compute_heavy.sig` | FP-heavy (~10 ops/load, sqrt chains) | **~2.6×** |
| `examples/stateless_heavy.sig` | Memory-bound (~3 ops/load) | **~1.4×** |

**Artifact:** `bench/results/avx2_speedup/` (README + per-program markdown + post-O2 IR dumps).

**Correctness:** `vectorized_lanes_parity_test` (including `compute_heavy_sqrt_chain`); `cross_symbol_benchmark` sink bit-equality.

**Mechanism:** `CompileProgramVectorized` widens stateless IR to `<K x double>`; LLVM O2 keeps vector ops through codegen. Stateful ops use per-lane scalarized fan-out (P10); with default `kAll` lowering, P4 emits per-lane inline IR (`LaneEmitScope`) — see `docs/cross_symbol_vectorization_stateful.md` and `bench/results/stateful_vec_lowering_speedup.md`.

## Reproduction

```bash
cd build-wsl
taskset -c 2 ./cross_symbol_benchmark ../examples/stateless_compute_heavy.sig 2000000 --lanes=4 --runs=15
```

## Resume wording

- Safe: “vectorized multi-symbol compilation achieves ~2.6× speedup over scalar JIT on compute-heavy stateless programs (K=4), parity-tested.”
- Avoid: “AVX2 always beats scalar” or quoting `results_simd.csv` without program context.
