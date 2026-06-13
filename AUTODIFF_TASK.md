# Worker Task: Reverse-Mode Automatic Differentiation over the Signal IR

> **Status (2026-06-12):** Not started. Spec origin: `improvements.md` §1 (recommended
> option). This brief supersedes that entry — follow this document.

> **You are an implementation agent.** This document is your complete brief. Execute it
> phase by phase. Do **not** skip the required reading, do **not** invent numbers, and do
> **not** weaken interpreter/JIT parity. Phase 0 ends in a **human checkpoint** — stop and
> report; do not write implementation code until the design is approved.

---

## 1. Mission (one sentence)

Add an autodiff pass that, given a signal program with named continuous parameters,
**constructs the derivative program as new IR and JIT-compiles it with the existing LLVM
pipeline**, then use the compiled gradients to calibrate parameters by gradient descent on
recorded data — with gradient correctness gated by finite-difference checks the same way
JIT correctness is gated by interpreter parity.

---

## 2. How to use this document

- Work **one phase at a time, in order.** Phase 0 is a design gate: produce the derivation
  document and **report before writing any implementation code.**
- After **every** phase, the full test suite must be green (§6) before moving on.
- Use the per-phase **Acceptance Criteria** as your definition of done. Each phase must
  produce a concrete, reproducible test or artifact — not just a code change.
- Report using the format in §11 after each phase.

---

## 3. Required reading (before touching any code)

In this order (per `CLAUDE.md` Read Order; it is fast and prevents wrong turns):

1. `CLAUDE.md` — project rules. **These override default behavior. Follow them exactly.**
2. `docs/agent_architecture.md`
3. `EVIDENCE.md` and `CLAIMS_MATRIX.md` — claim → artifact discipline you must extend
4. `context/PROJECT_CONTEXT.md`
5. `src/ast.h` — node kinds; how literals and operators are represented
6. `src/signal_program.h` — program build, inlining, **stable `node_id` allocation** (your
   adjoint state slots will ride on the same mechanism)
7. `src/runtime.h` — `SignalContext` state vectors (ema/rolling/vwap/lag state layout)
8. `src/interpreter.cpp` — reference semantics for every operator you will differentiate
   (especially `ema`, `kalman1d`, Welford `rolling_std`, `rolling_min/max`, `cross_*`)
9. `src/jit_compiler.h` — compile entry points; `StatefulLoweringFlags`; how a second
   emitted function would reuse the pipeline
10. `test/stateful_lowering_parity_test.cpp` — the parity-test pattern your
    `gradient_parity_test` must replicate

Then skim only the seams named in `CLAUDE.md` Exploration Discipline. Do not broadly
explore.

---

## 4. Ground truth (verified starting facts + items you must confirm)

**What exists and you will reuse**
- Typed AST/IR with stable `node_id`s for stateful ops (`signal_program`), an interpreter
  that is the semantic oracle, an LLVM ORC JIT (`jit_compiler`) with single-signal and
  fused `CompileProgram` paths, dense node-indexed state in `SignalContext` (`runtime`),
  and a persistent module cache.
- Stateful lowering is complete and default (`kAll`); parity suites
  (`stateful_lowering_parity_test`, `fuzz_parity_test`, `welford_stddev_parity_test`)
  define the tolerance discipline (benign ≤1e-12 rel; cancellation-prone ≤1e-6 rel).
- Recorded-data backtest pipeline exists (`backtest_runner`, IC reports under
  `bench/results/backtest/`), intended as the Phase 4 calibration objective.

**Operator inventory to differentiate** (from `src/interpreter.cpp`)
- Stateless: `+ - * /`, comparisons, `&& || !`, `if/then/else` (select), `abs`, `log`,
  `sqrt`, market reads (`mid/bid/ask/spread`).
- Linear windowed: `sma`, `lag`, `vwap` (linear in inputs ⇒ adjoint is a window scatter).
- Recurrent: `ema`, `kalman1d` (derivative state is itself a recurrence — the crux).
- Nonlinear windowed: `rolling_std` (Welford), `zscore` (chain rule via mean/std),
  `rolling_corr`, `rolling_beta`.
- Non-smooth: `rolling_min`, `rolling_max` (subgradient → arg-extremum),
  `cross_above`, `cross_below` (step functions → stop-gradient by default).

**CONFIRM in Phase 0 (do not build on these until verified):**
- **[CONFIRM-1] The recorded-data objective is currently broken at HEAD.** A 2026-06-10
  audit found `backtest_runner` emits `UNKNOWN` for every symbol, producing all-NaN IC
  reports with 0 samples (`bench/results/backtest/determinism_a/ic_report.json`). If this
  regression is still present, Phase 4's calibration objective must either (a) wait for the
  fix, or (b) use a self-contained objective on a checked-in CSV fixture (fit-to-target
  series). Verify, decide, and state the decision in the Phase 0 report.
- **[CONFIRM-2] There is no parameter mechanism yet.** Literals are baked into the AST.
  Confirm how literals are represented and design the parameter binding (§ Phase 0, D1).
  Do not silently make every literal differentiable.
- **[CONFIRM-3]** Whether `signal_program`'s structural dedup would merge two uses of the
  same parameter into one node (it should — adjoint accumulation must then **sum** over
  consumers; write a test for exactly this case).

---

## 5. Hypothesis you are testing

> The adjoint of a signal program is itself a signal-program-shaped computation (the
> derivative of a recurrence is another recurrence with its own per-node state), so it can
> be constructed as ordinary IR, allocated state slots via the existing `node_id`
> machinery, and compiled by the existing JIT — giving exact, native-speed gradients with
> no tape interpreter and no per-tick allocation.

The metric that matters: **compiled-gradient correctness vs central finite differences on
every differentiable op and on whole programs**, then a calibration run whose objective
measurably improves. Performance of the gradient path is secondary; correctness artifacts
are the deliverable.

---

## 6. Hard constraints (NON-NEGOTIABLE)

1. **Never change interpreter semantics.** The interpreter stays the value oracle. Autodiff
   is additive: new pass, new state slots, new emitted function. Zero behavior change for
   programs that use no parameters.
2. **Gradient parity is the new parity.** Every differentiable op and ≥3 whole programs
   must pass central-difference gradient checks within documented tolerances (§8 Phase 1
   sets them). The compiled (JIT) gradient must also match the interpreted adjoint to
   ≤1e-12 rel (same discipline as value parity).
3. **These suites must stay 100% green after every phase:** `fuzz_parity_test`,
   `fuzz_parity_simd_test`, `fuzz_parity_multisymbol_test`,
   `stateful_lowering_parity_test`, `hot_path_allocation_test`, `node_state_layout_test`,
   `differential_oracle_test`. Full `ctest` green before any phase is "done".
4. **Hot-path discipline:** the per-tick gradient update must be allocation-free after
   warmup (extend `hot_path_allocation_test` or add a sibling).
5. **Integer window `period`s are NOT differentiable.** Document this; do not implement
   smoothed surrogates unless the human asks.
6. **No fabricated numbers.** Any objective-improvement or throughput claim cites
   artifact + host + build + command (`EVIDENCE.md` style); ratios only on the pinned host
   (`bench/PINNED_HOST.md`).
7. **No commits or pushes** unless the human explicitly asks. Feature branch; clean tree;
   report.
8. **Documentation duty** (`CLAUDE.md`): when evidence changes, update `CLAIMS_MATRIX.md`,
   `EVIDENCE.md`, `context/PROJECT_CONTEXT.md`, `PROJECT_STATE.md`, `PROJECT_ROADMAP.md`,
   append `AUDIT_LOG.md`, update `NEXT_TASK.md`. `RESUME_CLAIMS.md` only on claim changes.

---

## 7. Build / test commands (WSL, LLVM-enabled — the canonical env)

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release -DJITSE_ENABLE_LLVM=ON
cmake --build build-wsl -j

ctest --test-dir build-wsl --output-on-failure
ctest --test-dir build-wsl -R gradient_parity_test --output-on-failure   # yours, Phase 1+
ctest --test-dir build-wsl -R fuzz_parity --output-on-failure
```

---

## 8. Phases

### Phase 0 — Design doc + derivations  *(GATE — human checkpoint, no implementation code)*

**Goal:** Resolve the design decisions on paper and prove the math before any code.

**Decisions to make (write each up with the rejected alternative and why):**

- **D1 — Parameter surface.** How a `.sig` author declares a differentiable parameter.
  Recommended: explicit `param` declarations (e.g. `param alpha = 0.1` referenced by name),
  lowered to a dense parameter vector in `SignalContext` (indexed like node state), read by
  both interpreter and JIT. Literals stay constants. Type-check arity/redeclaration.
- **D2 — Time direction.** True reverse-mode through time (BPTT) needs the state history
  of the whole run (O(T) memory or checkpointing) — wrong shape for a streaming per-tick
  engine. **Recommended: reverse-mode over the expression DAG within a tick, forward
  sensitivity propagation over time** (carry `∂state/∂θ_i` as extra per-node state — exact,
  RTRL-style, O(P) extra state, allocation-free, fits the engine). Cost grows with
  parameter count P; document the tradeoff and the P range it targets (≲ dozens). If you
  choose differently, justify it at the checkpoint.
- **D3 — Non-smooth policy.** `rolling_min/max`: subgradient routed to the arg-extremum.
  `cross_above/below` and comparison-driven `if`: stop-gradient (derivative 0) by default;
  note (don't build) the smooth-surrogate option.
- **D4 — Objective for Phase 4** given [CONFIRM-1]: recorded-data IC vs checked-in CSV
  fit-to-target fixture. Pick one primary; the fixture path must exist regardless (it's the
  reproducible CI-friendly artifact).

**Derivations to include (the math the human must be able to defend):**
- `ema`: from `e_t = α·x_t + (1−α)·e_{t−1}`, derive
  `∂e_t/∂α = (x_t − e_{t−1}) + (1−α)·∂e_{t−1}/∂α` and identify the new state slot.
- `kalman1d`: sensitivity recurrences of the predict/update equations w.r.t. its
  continuous parameters (process/measurement noise), matching `src/interpreter.cpp`'s
  exact update form.
- Welford `rolling_std` (and `zscore` via chain rule): differentiate the *same update
  equations the interpreter uses*, not a textbook variance formula.
- Fan-out rule: a node feeding multiple consumers **sums** incoming adjoints
  ([CONFIRM-3] interacts with dedup).

**Acceptance criteria**
- `docs/autodiff_design.md` containing D1–D4 (with rejected alternatives), all derivations,
  the new-state-slot inventory per op, and resolutions of [CONFIRM-1..3].
- **Checkpoint: stop and report.** Wait for human approval of the design doc before
  Phase 1.

---

### Phase 1 — Parameters + stateless adjoints, interpreter-evaluated

**Goal:** End-to-end gradients for stateless programs, with the gradient-check harness that
gates everything after.

**Steps**
1. Implement D1: `param` parsing (`lexer`/`parser`), program-level parameter table
   (`signal_program`), parameter vector in `SignalContext` (`runtime`), interpreter reads.
   JIT reads the same vector (values only — no derivatives yet). All existing suites green.
2. New `src/autodiff.{h,cpp}`: adjoint construction for the stateless subset
   (`+ - * /`, `abs`, `log`, `sqrt`, comparisons, select/if, market reads = constants
   w.r.t. θ). Output is ordinary program IR evaluated by the interpreter.
3. `test/gradient_parity_test.cpp`: central-difference harness. For each parameter,
   compare AD gradient vs `(f(θ+h) − f(θ−h)) / 2h` with per-parameter
   `h = 1e-6·max(1,|θ|)`; tolerance: rel ≤1e-4 (or abs ≤1e-8 near zero) for smooth ops —
   **document both constants in the test header and the design doc.** Include a fan-out
   case (one param used in two signals) and a `log`/`sqrt` domain-edge case.

**Acceptance criteria**
- `gradient_parity_test` green for every stateless op + ≥2 stateless whole programs.
- Zero diffs in all pre-existing suites; a no-`param` program compiles byte-identically
  (or measurably identically) to before.

---

### Phase 2 — Stateful adjoints  *(the crux)*

**Goal:** Sensitivity propagation through every stateful operator, per the Phase 0
derivations.

**Order (easiest → hardest, each gated by its gradient check before the next):**
1. `lag`, `sma`, `vwap` (linear: window scatter / delayed sensitivity).
2. `ema` (first true recurrence — new `∂e/∂θ` state slot allocated via the existing
   `node_id` machinery so layout stays dense; extend `node_state_layout_test`).
3. `zscore`, `rolling_std` (Welford sensitivities), then `rolling_corr`, `rolling_beta`.
4. `kalman1d`.
5. Non-smooth per D3: `rolling_min/max` subgradient, `cross_*` stop-gradient — each with a
   test asserting the documented convention (not just "doesn't crash").

**Numerical hazards:** match the interpreter's exact update ordering when differentiating;
warm-up regions (window not yet full) must have defined sensitivities — define them in the
design doc and test the boundary tick. Loosen the FD tolerance only for documented
cancellation-prone cases (mirror the value-parity precedent), never silently.

**Acceptance criteria**
- `gradient_parity_test` green for all ops above + ≥3 whole programs including one with
  `ema` and one with `kalman1d` (use `examples/filtered_momentum.sig` parameterized on its
  EMA decays and threshold as one of them).
- `node_state_layout_test` extended and green; full `ctest` green.

---

### Phase 3 — JIT-compile the adjoint

**Goal:** The gradient path is a compiled native function, not an interpreted one.

**Steps**
1. Emit the adjoint program through `jit_compiler` (reuse `CompileProgram` and the
   stateful-lowering machinery — the sensitivity recurrences are stateful ops like any
   other; reuse the module cache).
2. Three-way gate per program: JIT gradient ≡ interpreted adjoint (≤1e-12 rel) ∧ both pass
   FD checks.
3. Allocation-free warm gradient tick (extend `hot_path_allocation_test` or sibling).
4. Add parameterized-program cases to `fuzz_parity_test`'s generator so value parity is
   fuzzed with the param machinery active.

**Acceptance criteria**
- Compiled adjoint passes the three-way gate on the Phase 2 program set; allocation test
  green; fuzz green; full `ctest` green.
- A pinned-host measurement of compiled forward+gradient tick cost vs forward-only
  (artifact under `bench/results/autodiff/` with `*.meta.json` provenance — honest number,
  whatever it is).

---

### Phase 4 — Calibration driver  *(the demo)*

**Goal:** Use the compiled gradients to actually improve an objective.

**Steps**
1. Per D4: a `cli/jitse_calibrate` (or `examples/`) driver — load program + data, run Adam
   (document lr/β/iters) over the parameter vector using the compiled gradient, log the
   objective per iteration, write optimized params + trajectory.
2. Primary run per D4's choice; the CSV-fixture fit-to-target run regardless (checked-in,
   deterministic, becomes a `ctest` target: `calibration_smoke_test` asserting the
   objective improves by a fixed margin on the fixture).
3. Artifacts: `bench/results/autodiff/calibration_<objective>.md` — initial vs final
   objective, parameter table, trajectory, exact reproduce command, host fingerprint.

**Acceptance criteria**
- Measurable objective improvement on the fixture (gated by `calibration_smoke_test`) and,
  if [CONFIRM-1] is resolved, on recorded data — both honestly reported (if recorded-data
  IC barely moves, say so; the compiler is the claim, not the alpha).
- Full `ctest` green. Truth docs updated per §6.8; `AUDIT_LOG.md` appended.

---

### Phase 5 — (STRETCH, optional) Multi-parameter scaling note

Only if Phases 0–4 are fully green: measure gradient-tick cost vs parameter count P
(1, 4, 16) to characterize the D2 forward-sensitivity tradeoff; one small artifact.
Do **not** attempt BPTT/checkpointing — note it as future work.

---

## 9. Definition of done (overall)

- `param` surface implemented end-to-end; zero impact on non-parameterized programs.
- Adjoint construction for every operator in §4's inventory (or a documented, justified
  subset), each gradient-checked; compiled adjoint matches interpreted adjoint.
- Calibration demo with reproducible artifact + smoke test.
- All suites green; truth docs updated; `docs/autodiff_design.md` reflects what was built.

---

## 10. Facts vs Uncertainty (carry this discipline)

- **Verified:** the reuse surface in §4 (node_id machinery, lowering, parity suites,
  module cache) exists per `EVIDENCE.md`/`CLAIMS_MATRIX.md`.
- **Must confirm:** [CONFIRM-1] backtest symbol regression status; [CONFIRM-2] literal
  representation / param plumbing; [CONFIRM-3] dedup-vs-fan-out interaction.
- **Estimate only:** gradient-tick overhead, calibration improvement magnitude — Phase 3/4
  replace these with measurements. Never ship an estimate as a result.

---

## 11. Per-phase report format (use `CLAUDE.md` Final Response Contract)

1. **Result** — what changed; measured numbers with artifact + host + command.
2. **Files** — touched files + new artifacts.
3. **Verification** — exact test commands and outcomes (pass counts, tolerance values hit).
4. **Facts vs Uncertainty** — measured vs assumed; anything needing human confirmation.
5. **Next Prompt** — next phase or blockers.

---

## 12. Why this matters (context — NOT a license to fabricate)

Target resume bullet (numbers TBD by building it):

> *Implemented reverse-mode automatic differentiation as a compiler pass over a trading-DSL
> IR — constructing and JIT-compiling derivative programs for recurrent operators (EMA,
> Kalman, Welford rolling statistics) with sensitivity state carried per tick — validated
> against finite-difference gradient checks and used to calibrate signal parameters by
> gradient descent on recorded market data.*

Every number in the final wording must trace to an artifact (`EVIDENCE.md` discipline).
A truthful modest result beats an impressive fabricated one.
