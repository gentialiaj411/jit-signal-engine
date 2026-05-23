# CSE Diff Report (Phase 1)

## Program and Function
- Program path: `examples/filtered_momentum.sig`
- Compilation mode: `--all-signals` (fused `CompileProgram` path)
- Function: `@signal_program_func_1`

## Environment
- Host path: Windows 11 workspace via WSL2 Ubuntu
- Git commit SHA: `cce2c3df0f9f9938308276919d1a7f1de4656cfb`
- LLVM detected by CMake: `18.1.8`
- JIT availability during capture: `llvm_jit_available=true`

## Artifacts
- `bench/results/cse_evidence/before.ll` (pre-opt IR)
- `bench/results/cse_evidence/after.ll` (post-O2 IR)

## Load Count Result (relevant market-data pointers)
Counting bid/ask loads for the same symbol in the fused function:

- Bid loads before: `11` (`load double, ptr %mid_bid_ptr...`)
- Bid loads after: `11` (`load double, ptr %market`)
- Ask loads before: `11` (`load double, ptr %mid_ask_ptr...`)
- Ask loads after: `11` (`load double, ptr %mid_ask_ptr`)
- Total relevant loads before: `22`
- Total relevant loads after: `22`

## Additional IR Change Observed
- Repeated market-struct GEP chains were simplified:
  - Before: `22` occurrences of `getelementptr ... ptr %market ...`
  - After: `0` occurrences of that full chain pattern; optimized IR reuses `%market` and `%mid_ask_ptr`.

## Conclusion
For this fused case, post-O2 IR shows **address/pointer simplification** but **no measurable reduction in market-data load instruction count**.
Therefore, "market-data read deduplication via CSE" remains **Supported** (architectural), not **Verified** by load-count delta.
