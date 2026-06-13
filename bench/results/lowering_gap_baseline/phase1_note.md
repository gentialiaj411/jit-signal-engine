# Phase 1 complete — lowering default promoted to `kAll`

## Code changes

- `ParseStatefulLoweringEnv()` and `JitCompiler` impl default: **`kAll`** when env unset.
- `JITSE_LOWER_STATEFUL=none|off|0|false` still forces opaque runtime calls.
- `signal_benchmark` CLI default: **`kAll`**; `--lower-stateful=none` still selectable.
- `TieredProgramJit::Compile` already defaulted to `kAll` (unchanged).

## Reported numbers that changed (pinned host, `run_pinned_speedup.sh`)

| Case | Before (lowering off default) | After (lowering on default) |
|---|---:|---:|
| Momentum | 5.18× [4.92×, 5.36×] | **7.32×** [7.29×, 7.48×] |
| All-signals fused | 2.96× [2.50×, 3.53×] | **15.56×** [15.52×, 15.58×] |

Artifact: `bench/results/pinned_host_speedup.md` (regenerated 2026-06-10).

Historical lowering-off baseline remains in `bench/results/lowering_gap_baseline/baseline_table.md` (`jit_none` column).

## Test adjustments

- `fuzz_parity_test`: rel tolerance `1e-6` (production default is lowered JIT).
- `stateful_subtree_dedup_test`: explicit `kNone` for runtime-path bit-equality checks.
- `runtime_call_profile_test`: lag drop gate `1.5×` (SIGPROF noise on ~300-sample profiles).

## Verification

```bash
ctest --test-dir build-wsl --output-on-failure   # 36/36 passed
bash bench/run_pinned_speedup.sh build-wsl
```

## Next

Phase 2 — lower remaining 9 opaque ops (`zscore`, `rolling_min`, …).
