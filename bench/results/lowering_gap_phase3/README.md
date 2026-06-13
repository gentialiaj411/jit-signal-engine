# Lowering gap Phase 3 artifacts

Pinned host (`bench/pinned_host_spec.json`, core 2).

| File | Contents |
|---|---|
| `phase3_table.md` | JIT÷hw gap per signal (honest `hw_reference` baselines) |
| `phase3_conclusion.md` | Before/after vs Phase 0, perf profile, deferred items |
| `matrix.csv` | Raw measurements |
| `ir/README.md` | Confirms no `jit_rt_*_lowered_base` in fused IR |

```bash
bash bench/run_lowering_gap_phase3.sh build-wsl
```
