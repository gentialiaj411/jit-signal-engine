# Worker Task: Stateful-Operator IR Lowering — Close the JIT ↔ Hand-Written-C++ Gap

> **Status (2026-06-09):** Phases **0–4 complete**. Phase 3 summary:
> `bench/results/lowering_gap_phase3/phase3_conclusion.md` (fused JIT÷hw **1.42×**).
> Phase 4: parity green (`vectorized_stateful_parity_test` + `kAll`); perf on
> `filtered_momentum` — vec+lowered **2.13×** vs vec+opaque but **0.57×** vs
> scalar-lowered (`bench/results/stateful_vec_lowering_speedup.md`). True K-wide SIMD
> ring-buffer state not implemented.

> **You are an implementation agent.** This document is your complete brief. Execute it
> phase by phase. Do **not** skip the required reading, do **not** invent benchmark
> numbers, and do **not** weaken interpreter/JIT parity. When in doubt, prefer the
> narrowest reproducing test and report rather than guess.

---

## 1. Mission (one sentence)

The JIT already supports inline-IR "stateful lowering" for 4 operators but it is **off by
default** and **incomplete**, so compiled signals run ~3–6× slower than hand-written C++
on the fused path. **Make lowering the default, extend it to every stateful operator, and
close the gap to hand-written C++ — without ever breaking parity.**

---

## 2. How to use this document

- Work **one phase at a time, in order.** Phase 0 is a gate: complete it and **report the
  baseline table before writing any optimization code.** If the measured baseline
  contradicts the assumptions in §5, stop and surface it.
- After **every** phase, the full test suite must be green (§6) before moving on.
- Use the per-phase **Acceptance Criteria** as your definition of done. Each phase must
  produce a concrete, reproducible artifact or test — not just a code change.
- Report using the format in §11 after each phase.

---

## 3. Required reading (before touching any code)

Read in this order (from `CLAUDE.md` "Read Order"); it is fast and prevents wrong turns:

1. `CLAUDE.md` — project rules. **These override default behavior. Follow them exactly.**
2. `docs/agent_architecture.md`
3. `EVIDENCE.md` — claim → artifact map
4. `context/PROJECT_CONTEXT.md`
5. `CLAIMS_MATRIX.md` — verification status of every claim
6. `src/jit_compiler.h` — the `StatefulLoweringFlags` enum and the vectorization restriction comment (~lines 14–115)
7. `bench/results/perf/filtered_momentum/profile_lowering_all.txt` and `profile_lowering_none.txt`
8. `bench/results/rolling_std_lowering_speedup.md`
9. `test/stateful_lowering_parity_test.cpp` — the parity pattern you must replicate for new ops

Then skim the JIT seams named in `CLAUDE.md` "Exploration Discipline":
`signal_program`, `runtime`, `interpreter`, `jit_compiler`, `jit_test`, `fuzz_parity_test`.

---

## 4. Ground truth (already verified — start from these)

These were confirmed from the repo. Treat them as starting facts, but **re-verify the two
"CONFIRM" items in Phase 0** before quoting any number publicly.

**Lowering mechanism**
- `src/jit_compiler.h` defines `enum class StatefulLoweringFlags { kNone, kSma, kEma, kLag, kRollingStd, kAll }`.
  `kAll` today = **only** `sma | ema | lag | rolling_std`.
- **Default is `kNone`** — every stateful op is emitted as an opaque `extern "C"` call to a
  `jit_rt_*` function (registered via `getOrInsertFunction(...)` + `absoluteSymbols(...)` in
  `src/jit_compiler.cpp`, ~lines 1806–1934 and parallel blocks). LLVM cannot inline, CSE, or
  vectorize across these calls.
- Lowered ops emit inline IR and fetch their state via `jit_rt_<op>_lowered_base` helpers
  (e.g. `jit_rt_sma_lowered_base`, `jit_rt_rolling_std_lowered_base`).
- Methods: `JitCompiler::SetStatefulLowering(flags)` / `GetStatefulLowering()`
  (`src/jit_compiler.h` ~173). Note `TieredProgramJit` already defaults its baseline to `kAll`
  (~line 330) while other entry points default to `kNone` — **the default is inconsistent
  across entry points; you will unify this in Phase 1.**

**Operators still emitted as opaque calls (NOT yet lowered) — the Phase 2 worklist:**
`zscore`, `rolling_min`, `rolling_max`, `vwap`, `cross_above`, `cross_below`,
`rolling_corr`, `rolling_beta`, `kalman1d`.

**Performance evidence (why this matters)**
- `perf` on fused `filtered_momentum.sig`, lowering **off**: ~**69.6%** of cycles in
  `jit_rt_rolling_std`, ~8% in `jit_rt_ema_alpha` (~78% trapped in runtime calls); only ~16%
  in compiled code. (`profile_lowering_none.txt`)
- Same program, lowering **on**: ~**83%** in compiled code, runtime calls down to ~2%, and
  ~2.7× fewer total samples. (`profile_lowering_all.txt`)
- Lowered fused path measured at **15.6× vs interpreter** (`rolling_std_lowering_speedup.md`),
  versus the canonical **2.96×** fused number reported with lowering off
  (`bench/results/pinned_host_speedup.md`).

**Hand-written-C++ baseline (the real target)**
- `bench/signal_benchmark.cpp` emits a hardcoded C++ path as the `hw_*` columns in
  `bench/results.csv`. On that artifact, JIT-vs-hardcoded gaps were `spread` ~1.45×,
  `spread_z` ~3.5×, `<all_signals>` ~6.2×. Some signals (`momentum`, `z`, `dev`) have **no**
  hw baseline (`nan`).

**Vectorization + lowering (Phase 4 — complete)**
- P4 lifts the former mutual exclusion via per-lane `LaneEmitScope` lowered fan-out
  (`src/jit_compiler.cpp`). Parity: `vectorized_stateful_parity_test` + `vectorized_lanes_parity_test`
  under `kAll`. Perf artifact: `bench/results/stateful_vec_lowering_speedup.md` — vec+lowered
  **2.13×** vs vec+opaque, **0.57×** vs scalar-lowered on `filtered_momentum` (K=4). True K-wide
  SIMD ring-buffer state not implemented.

**CONFIRM in Phase 0 (do not quote these as fact until verified):**
- **[CONFIRM-1]** That `bench/results.csv` was generated with lowering **off** (check
  `bench/results.csv.meta.json` and/or regenerate). The "~6× gap" assumes this.
- **[CONFIRM-2]** That the `hw` hardcoded path is a **fair** ceiling: real per-tick state
  updates, same warmup/batch as JIT, compiled at `-O2`/`-O3`, not precomputed. If it cheats,
  fix it before using it as the baseline.

---

## 5. Hypothesis you are testing (so you optimize the right thing)

> Most of the JIT↔hardcoded gap is opaque `jit_rt_*` call overhead + optimization barriers.
> Lowering all stateful ops to inline IR, then removing the residual `_lowered_base` calls
> and reusing shared rolling windows across signals, should bring the fused path to within
> ~1.X× of hand-written C++.

The metric that matters: **JIT throughput and p50/p99 latency on the pinned host, relative
to (a) the interpreter and (b) the hand-written C++ baseline — especially the fused
`<all_signals>` path.** Do not chase micro-optimizations that do not move those.

---

## 6. Hard constraints (NON-NEGOTIABLE)

1. **Parity is sacred.** Interpreter is the semantic reference. Every lowered op must match
   the interpreter and its `jit_rt_*` reference within the tolerances used by
   `stateful_lowering_parity_test` and `welford_stddev_parity_test`
   (benign ≤1e-12 rel; cancellation-prone ≤1e-6 rel). **Never** change interpreter semantics
   to make the JIT match.
2. **Keep `kNone` fully functional.** It is the differential-oracle/parity reference. Lowering
   is an additive code path, not a replacement.
3. **These suites must stay 100% green** after every phase (LLVM build):
   `stateful_lowering_parity_test`, `fuzz_parity_test`, `fuzz_parity_simd_test`,
   `fuzz_parity_multisymbol_test`, `differential_oracle_test`, `backtest_determinism_test`,
   `node_state_layout_test`, `hot_path_allocation_test`.
4. **No fabricated numbers.** Only report metrics you measured. All performance numbers are
   artifact-specific and must cite artifact + host + build type + command (per `EVIDENCE.md`).
   Speedup ratios are valid **only** on the pinned host (`bench/PINNED_HOST.md`).
5. **Do not commit or push** unless the human explicitly asks (`CLAUDE.md` Forbidden
   Behaviors). Work on a feature branch; leave a clean working tree + a report.
6. **Do not break** the multi-symbol or vectorized paths (until Phase 4 deliberately changes
   the lowering↔SIMD interaction, and even then parity tests gate it).
7. **Documentation duty** (`CLAUDE.md`): when measured behavior/evidence changes, update
   `CLAIMS_MATRIX.md`, `EVIDENCE.md`, `context/PROJECT_CONTEXT.md`, `PROJECT_STATE.md`,
   `PROJECT_ROADMAP.md`, append `AUDIT_LOG.md`, update `NEXT_TASK.md`. Update
   `RESUME_CLAIMS.md` only if claim wording/evidence status changes.

---

## 7. Build / test / bench commands (WSL, LLVM-enabled — the canonical env)

```bash
# Configure + build (Release, LLVM ON)
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release -DJITSE_ENABLE_LLVM=ON
cmake --build build-wsl -j

# Full test suite (run serially when debugging flakes)
ctest --test-dir build-wsl --output-on-failure
ctest --test-dir build-wsl -R stateful_lowering_parity_test --output-on-failure
ctest --test-dir build-wsl -R fuzz_parity --output-on-failure

# Benchmark a program, choosing lowering mode (flag confirmed in rolling_std_lowering_speedup.md)
./build-wsl/signal_benchmark --all-signals --pin-core 2 --measure-runs 30 --lower-stateful=none
./build-wsl/signal_benchmark --all-signals --pin-core 2 --measure-runs 30 --lower-stateful=all

# Canonical pinned speedup driver (validates host fingerprint before running)
bash bench/run_pinned_speedup.sh build-wsl

# perf profile (compare symbol attribution before/after lowering)
#   reuse the methodology that produced bench/results/perf/filtered_momentum/profile_lowering_*.txt
```

Verify the exact `signal_benchmark` flag names and the hardcoded-path coverage in
`bench/signal_benchmark.cpp` before relying on them; extend the flag wiring/baseline if a
signal or mode is missing.

---

## 8. Phases

### Phase 0 — Baseline & gap quantification  *(GATE — do first, then report and checkpoint)*

**Goal:** Replace estimates with a real before-table and locate the gap precisely.

**Steps**
1. Build (LLVM on). Run full `ctest` once to confirm a green starting point; record the count.
2. Resolve **[CONFIRM-1]** and **[CONFIRM-2]** from §4.
3. Ensure a hand-written C++ baseline exists for **every** benchmarked signal (add the
   missing `momentum`/`z`/`dev` hardcoded variants in `bench/signal_benchmark.cpp` if absent;
   keep them honest per CONFIRM-2).
4. On the pinned host (core 2, `--measure-runs 30`), measure each signal and the fused
   `<all_signals>` path across **four** paths: interpreter, JIT `lowering=none`, JIT
   `lowering=all`, hardcoded C++. Record throughput + p50/p99/p999.
5. Capture a `perf` symbol profile per signal for JIT `lowering=none` and `lowering=all`;
   attribute the residual gap to specific `jit_rt_*` symbols (this orders the Phase 2 worklist).

**Acceptance criteria**
- New artifact dir `bench/results/lowering_gap_baseline/` containing: a markdown table
  (signal × {interp, jit_off, jit_on, hardcoded} → throughput, p50, p99, p999), a derived
  **jit_on ÷ hardcoded gap ratio** per signal, the per-signal perf top-symbols, and the exact
  reproduce commands + host fingerprint.
- A short written conclusion: real current gap (with lowering on) and which `jit_rt_*` calls
  dominate the remainder.
- **Checkpoint:** report this table. If gaps roughly match §4 (jit_on ~2–4× off hardcoded on
  fused), proceed. If not, stop and surface the discrepancy.

---

### Phase 1 — Promote lowering to the production default

**Goal:** Make the fast, parity-tested path the default everywhere.

**Steps**
1. Audit **all** compile entry points for their lowering default: `CompileSignal`,
   `CompileProgram`, the vectorized compile, and `TieredProgramJit`. Unify the production
   default to `kAll`. Keep `kNone` reachable (explicit flag/arg) for the oracle and parity tests.
2. Flip the `signal_benchmark` default accordingly (keep `--lower-stateful=none` selectable).
3. Run the full suite. Investigate any parity/fuzz movement before proceeding.

**Acceptance criteria**
- Full `ctest` green, with the §6.3 suites explicitly passing.
- `kNone` path still compiles and still used by the differential oracle / parity tests.
- A note listing which reported numbers change (e.g. fused JIT-vs-interp 2.96× → measured
  `lowering=all` value). Update `CLAIMS_MATRIX.md`/`EVIDENCE.md` accordingly.

---

### Phase 2 — Lower the remaining stateful operators

**Goal:** Emit inline IR for all 9 currently-opaque ops (§4 worklist), ordered by the Phase 0
perf attribution (expect `zscore`, `rolling_min`, `rolling_max`, `vwap` first — they explain
the slow `z`/`spread_z`/`dev` signals).

**Per-operator procedure (repeat for each op)**
1. Add a flag bit to `StatefulLoweringFlags` and include it in `kAll`.
2. Emit inline IR in `src/jit_compiler.cpp` mirroring the op's `jit_rt_*` body, reusing the
   `jit_rt_<op>_lowered_base` state-pointer pattern (add a `_lowered_base` helper in
   `src/runtime.cpp` if one doesn't exist).
3. Add a dedicated case to `test/stateful_lowering_parity_test.cpp` asserting numerical
   equivalence to the `jit_rt_*` reference (and thus the interpreter) within documented tolerance.
4. Confirm `fuzz_parity_test` covers programs using the op under `kAll` (extend its generator
   if needed).
5. Run targeted tests, then the full suite.

**Numerical hazards:** `rolling_std` (Welford), `rolling_corr`, `rolling_beta`,
`kalman1d` — match the existing reference implementations exactly; do not reorder
floating-point ops in ways that break the tolerance gates.

**Acceptance criteria**
- Every op in the worklist is lowered, each with a passing parity case; `fuzz_parity_test`
  green under default (`kAll`).
- Re-run the Phase 0 benchmark matrix; show before/after for `z`, `spread_z`, `dev`, and fused.
- If you stop early (e.g. only the hot ops), document which ops remain opaque and why.

---

### Phase 3 — Close the residual gap to hand-written C++  *(the headline phase)*

**Goal:** Get the fused path within ~1.X× of hand-written C++.

**Steps (profile-driven — let `perf` + IR dumps choose the order)**
1. Remove residual call overhead: inline/hoist the `jit_rt_<op>_lowered_base` state-pointer
   helpers so state addresses live in registers across a tick instead of behind a call/barrier.
2. **Cross-signal subexpression reuse:** when multiple signals read the same rolling window
   (same op + same input + same period), compute it once. This extends the existing
   market-load dedup (`bench/results/cse_evidence/cse_diff_verified.md`, 22→2 loads) from
   *loads* to *compute*. Demonstrate with an IR diff like the existing CSE evidence.
3. Use the existing `assume_warm` `JitProfile` (P1) / `ComputeProgramWarmupThreshold` to prune
   steady-state guard branches in the hot loop where safe.
4. Inspect `DumpLastIR` and generated x86-64 to find spills/branches/missed vectorization;
   fix the highest-cost items.

**Acceptance criteria**
- Re-run perf: residual `jit_rt_*` / `_lowered_base` overhead materially reduced vs Phase 2.
- New gap table vs hardcoded C++ on the pinned host, with the before (Phase 0) and after
  numbers side by side. State the achieved fused gap honestly (e.g. "~3× → ~1.4×").
- Parity suites still green. An IR-diff artifact proving cross-signal window reuse.
- Update `CLAIMS_MATRIX.md`, `EVIDENCE.md`, `context/PROJECT_CONTEXT.md`, `PROJECT_STATE.md`,
  `PROJECT_ROADMAP.md`; append `AUDIT_LOG.md`; update `NEXT_TASK.md`.

---

### Phase 4 — (STRETCH, optional) Compose lowering with cross-symbol vectorization

**Goal:** Lift the mutual exclusion (`src/jit_compiler.h` ~90–96) so stateful ops can be both
inlined **and** SIMD-vectorized across K symbols.

**Steps**
- Widen the lowered ring-buffer IR to `<K x double>` with per-lane state base pointers
  (gather/scatter or a K-wide state layout). This is the hard part the comment calls out.
- Remove the "vectorized compile rejects lowered op" guard only once parity holds.

**Acceptance criteria**
- `fuzz_parity_simd_test` and `vectorized_lanes_parity_test` pass with lowering enabled. **Done.**
- A benchmark showing a lowered **and** vectorized program beating both the scalar-lowered and
  the vectorized-unlowered paths on an FP-heavy stateful program (cite the artifact).
  **Partial:** beats vec+opaque (**2.13×**); does **not** beat scalar-lowered (**0.57×**) —
  documented in `bench/results/stateful_vec_lowering_speedup.md`. Guard removed; per-lane
  duplication is the perf ceiling without K-wide SoA state.

---

## 9. Definition of done (overall)

- Lowering is the default across all compile entry points; `kNone` remains reachable.
- All 9 worklist ops lowered (or a documented, perf-justified subset), each parity-gated.
- Fused-path gap to hand-written C++ measured and materially reduced, with before/after
  artifacts on the pinned host.
- All §6.3 suites green; full `ctest` green.
- Truth docs updated per §6.7; `AUDIT_LOG.md` appended with what changed and the new evidence.

---

## 10. Facts vs Uncertainty (carry this discipline)

- **Verified (from artifacts):** all 14 stateful ops lowered; default `kAll`; fused JIT÷hw
  **1.42×**; `jit_rt_*` **80%→2.8%** on `filtered_momentum`; P4 parity green; vec+lowered
  **2.13×** vs vec+opaque, **0.57×** vs scalar-lowered (K=4). Historical: lowering-off fused
  **2.96×** vs interpreter (`lowering_gap_baseline/`).
- **Must confirm before quoting:** `results.csv` lowering provenance [CONFIRM-1]; hardcoded
  baseline fairness/coverage [CONFIRM-2]; exact per-entry-point lowering defaults; exact
  `signal_benchmark` flag wiring.
- **Estimate only (replace with measurement):** "~6× → ~1.X×" gap framing. The "before" is
  Phase 0's job; the "after" is Phase 3's. Never ship the estimate as the result.

---

## 11. Per-phase report format (use `CLAUDE.md` Final Response Contract)

1. **Result** — what changed, and the measured number (with artifact + host + command).
2. **Files** — touched files + new artifacts.
3. **Verification** — exact test/bench commands run and their outcome (pass counts, deltas).
4. **Facts vs Uncertainty** — what's measured vs assumed; anything that needs human confirmation.
5. **Next Prompt** — the next phase to run, or blockers.

---

## 12. Why this matters (context — NOT a license to fabricate)

The downstream goal is a resume bullet of the form:

> *Eliminated a hot path that `perf` showed spending ~78% of cycles inside opaque runtime
> calls by lowering all rolling/stateful operators to inline LLVM IR with cross-signal
> subexpression reuse — cutting the fused multi-signal gap to hand-written C++ from ~Nx to
> ~1.Xx and raising JIT-vs-interpreter throughput on the fused program from ~3× to ~15×.*

The `N` and `1.X` must be **your real measured numbers** from Phases 0 and 3 on the pinned
host. A truthful smaller win beats an impressive fabricated one — the whole repo is built on
claim→artifact traceability (`EVIDENCE.md`, `CLAIMS_MATRIX.md`). Keep it that way.
