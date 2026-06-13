# AGENTS.md

## Purpose
`jit-signal-engine` is a C++20 signal-evaluation engine with a custom DSL.
It supports interpreter and LLVM ORC JIT execution paths, plus whole-program fusion (`CompileProgram`), stateful-operator IR lowering (default `kAll`), cross-symbol vectorization (`CompileProgramVectorized`), AVX2-gated SIMD lowering, multi-symbol SoA execution, and recorded-data backtesting.
It parses `.sig` programs, inlines dependencies, assigns node IDs for stateful ops, prewarms runtime state, and evaluates per tick.
Its main performance shape is whole-program JIT fusion and dense node-indexed state.

## Read Order (Fast Onboarding)
1. `context/BATON.md`
2. `context/PROJECT_CONTEXT.md`
3. `docs/agent_architecture.md`
4. `EVIDENCE.md`
5. `context/EVIDENCE_MAP.md`
6. `CLAIMS_MATRIX.md`
7. `PROJECT_STATE.md`
8. `README.md` and `docs/architecture.md` only when needed
9. `context/DEEP_CONTEXT.md` only if blocked

## When To Inspect Source
- Read source only when context docs cannot answer the question.
- For resume/claim tasks, avoid source scans unless verifying one specific claim.
- Start at the smallest relevant file set (`signal_program`, `runtime`, `interpreter`, `jit_compiler`, matching tests).
- Stop once enough evidence exists to patch or report uncertainty.

## Build/Test/Benchmark Commands
Build:
- `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Tests:
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -R jit_test --output-on-failure`
- `ctest --test-dir build -R fuzz_parity_test --output-on-failure`

Bench:
- `bash bench/run_pinned_speedup.sh [build_dir]` — canonical JIT-vs-interpreter speedup (pinned host only)
- `bash bench/run_pinned_multithread_scaling.sh [build_dir]` — pinned multi-threaded multi-symbol scaling
- `bench/cross_symbol_benchmark` — vec vs scalar JIT (`bench/results/avx2_speedup/`)
- `bench/latency_bench` — per-call latency + CO-aware mode (`bench/results/latency/`)
- `bench/spsc_jit_pipeline_bench` — SPSC ingest pipeline (`bench/results/spsc_pipeline/`; Linux, env CPU pins)
- `bash bench/run_lowering_gap_phase3.sh [build_dir]` — JIT vs hand-written C++ gap (`bench/results/lowering_gap_phase3/`)
- `bash bench/run_stateful_vec_lowering_phase4.sh [build_dir]` — P4 lowered+vec three-way bench (`bench/results/stateful_vec_lowering_speedup.md`)
- `bash bench/run_benchmarks.sh [build_dir] [out_csv] [pin_core] [events]`
- `.\bench\run_benchmarks.ps1 -BuildDir .\build\Release -OutCsv .\bench\results.csv -PinCore 0 -Events 1000000`

Notes:
- JIT availability is environment-dependent (`LLVM_FOUND` + `JITSE_ENABLE_LLVM=ON`).
- Treat benchmark numbers as valid only with matching environment/protocol.

## Coding Rules
- Keep edits surgical and behavior-scoped.
- Do not change DSL semantics, node ID allocation rules, or runtime helper contracts without tests.
- Preserve interpreter/JIT parity as a hard invariant.
- Do not widen project scope claims beyond signal DSL + interpreter/JIT engine.

## Documentation Update Rules
After meaningful changes:
- `CLAIMS_MATRIX.md`: update evidence/risk/status lines.
- `context/EVIDENCE_MAP.md`: update supported vs risky claim boundaries.
- `context/PROJECT_CONTEXT.md`: update architecture/command truth.
- `PROJECT_CONTEXT.md` and `PROJECT_STATE.md`: keep the repo-facing state summaries aligned with evidence.
- `AUDIT_LOG.md`: append concise worklog.
- `NEXT_TASK.md`: update baton next action.
- `README.md`: update only externally visible behavior.

## Resume/Evidence Rules
- Use only evidence-backed claims from `CLAIMS_MATRIX.md`, `context/EVIDENCE_MAP.md`, `EVIDENCE.md`, and the benchmark artifacts.
- Separate verified facts from architectural/support-only claims.
- Keep uncertainty explicit as `TODO/VERIFY`.
- Do not invent speedups, latency numbers, or portability claims.

## Forbidden Behaviors
- No broad repo scans by default.
- No unrelated rewrites/refactors.
- No claims that imply universal JIT speedup on all hosts.
- No commits or branch manipulation unless explicitly requested.

## Final Response Format
1. `Changes`
2. `Verification`
3. `Evidence Notes` (Verified vs TODO/VERIFY)
4. `Next Step`

## Escalation Rules
Escalate when:
- docs conflict on benchmark/claim wording,
- LLVM/JIT availability changes expected behavior,
- requested claim lacks direct proof,
- parity risk exists after touching parser/runtime/JIT seams.

Escalation output must include:
- `Verified`
- `Unverified`
- `Assumptions`
- `Minimal check to resolve`
