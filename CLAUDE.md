# CLAUDE.md

## Purpose
`jit-signal-engine` is a C++20 DSL runtime/compiler for trading signals.
It has an interpreter baseline and an LLVM ORC JIT backend.
It supports single-signal native compilation, whole-program fusion (`CompileProgram`), AVX2-gated SIMD lowering, multi-symbol SoA execution, and deterministic recorded-data backtesting.
It is a focused signal engine, not a general-purpose compiler platform.

## Read Order
1. `context/PROJECT_CONTEXT.md`
2. `context/EVIDENCE_MAP.md`
3. `CLAIMS_MATRIX.md`
4. `PROJECT_STATE.md`
5. `README.md`
6. `docs/architecture.md`
7. `context/DEEP_CONTEXT.md` only if necessary

## Exploration Discipline
- Prefer docs-first reasoning.
- Do not explore broad source areas by default.
- Open source only to resolve concrete ambiguity or implement bounded edits.
- Prioritize these seams when debugging:
  - `signal_program` (parse/inlining/node IDs)
  - `runtime` (state sizing/helpers)
  - `interpreter` (semantic oracle)
  - `jit_compiler` (lowering/symbol registration)
  - `jit_test` and `fuzz_parity_test` (parity evidence)

## Reasoning Rules
- Keep interpreter semantics as correctness baseline.
- Treat interpreter/JIT parity as non-negotiable.
- Distinguish:
  - `Verified`: test/artifact-backed
  - `Supported`: architectural but not independently measured
  - `Unverified`: TODO/VERIFY
- Avoid broad claims about CSE or speedups without direct artifacts.
- Treat benchmark claims as artifact-specific, not universal.

## Build/Test/Bench Commands
Build:
- `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Test:
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -R jit_test --output-on-failure`
- `ctest --test-dir build -R fuzz_parity_test --output-on-failure`

Bench:
- `bash bench/run_benchmarks.sh [build_dir] [out_csv] [pin_core] [events]`
- `.\bench\run_benchmarks.ps1 -BuildDir .\build\Release -OutCsv .\bench\results.csv -PinCore 0 -Events 1000000`

## Debugging Strategy
1. Reproduce in the narrowest test.
2. Isolate parser vs runtime vs interpreter vs JIT lowering.
3. Check node ID allocation and prewarm assumptions before deeper changes.
4. Patch minimally.
5. Re-run targeted tests, then full relevant set.

## Documentation and Evidence Rules
When behavior/evidence changes:
- update `CLAIMS_MATRIX.md`,
- update `context/PROJECT_CONTEXT.md` if command/architecture truth changes,
- update `PROJECT_CONTEXT.md` and `PROJECT_STATE.md` if the top-level repo-facing summary changes,
- append `AUDIT_LOG.md`,
- update `NEXT_TASK.md`,
- update `RESUME_CLAIMS.md` only if claim wording/evidence status changes.

## Resume Positioning Rules
- Only provide resume wording when asked.
- Keep bullets technical but readable; avoid unnecessary jargon.
- Anchor claims to concrete evidence (`bench/results.csv`, parity tests, benchmark scripts).
- Do not claim universal latency/speedup portability.

## Forbidden Behaviors
- No broad repo exploration by default.
- No rewriting docs for style-only changes.
- No conflating architecture intent with measured evidence.
- No commit actions unless explicitly requested.

## Final Response Contract
1. `Result`
2. `Files`
3. `Verification`
4. `Facts vs Uncertainty`
5. `Next Prompt` (optional)

Keep outputs concise and action-oriented.

## Escalation Rules
Escalate when:
- LLVM availability changes expected behavior,
- requested claim exceeds evidence,
- docs disagree on benchmark/claim numbers,
- parity may regress.

Escalation format:
- `Verified`
- `Unverified`
- `Assumptions`
- `Minimal resolution plan`
