# improvements.md

Catalog of substantial, **net-new** capabilities that could be added to `jit-signal-engine`.
This is a menu for dispatching implementation agents — not a backlog of polish. Each entry is
spec'd in enough detail that an LLM worker can understand exactly what is meant and expand it
into a full phased brief.

> When an option is chosen, expand it into a standalone worker brief in the style of
> `OPERATOR_LOWERING_TASK.md` (mission → required reading → ground truth → hard constraints →
> phased plan with acceptance criteria → reporting format).

---

## How agents must use this file

**Shared ground rules for every entry (do not restate per item — they always apply):**

1. **Read `CLAUDE.md` first.** Its rules override default behavior. Follow the Read Order, the
   Exploration Discipline (don't broadly explore; use the named seams), and the Final Response
   Contract for reporting.
2. **Parity is non-negotiable.** The interpreter is the semantic reference. Any new compiled
   path must match it (and the existing `jit_rt_*` references) within the tolerances used by
   `stateful_lowering_parity_test` / `welford_stddev_parity_test`. Never change interpreter
   semantics to make a new path agree.
3. **No fabricated numbers.** Every performance claim cites artifact + host + build + command.
   Speedup/latency ratios are valid **only** on the pinned host (`bench/PINNED_HOST.md`).
4. **Don't break existing paths** (single-signal JIT, fused `CompileProgram`, cross-symbol
   vectorization, multi-symbol SoA, multi-thread) — gated by their fuzz/parity tests.
5. **No commits or pushes** unless the human explicitly asks. Work on a branch; leave a clean
   tree + a report.
6. **Documentation duty:** when measured behavior/evidence changes, update `CLAIMS_MATRIX.md`,
   `EVIDENCE.md`, `context/PROJECT_CONTEXT.md`, `PROJECT_STATE.md`, `PROJECT_ROADMAP.md`,
   append `AUDIT_LOG.md`, update `NEXT_TASK.md`; update `RESUME_CLAIMS.md` only if claim
   wording/evidence status changes.

**Per-entry template:** What it is · Why it's impressive · Build-on (current state) · Scope ·
Hard parts · Validation · Acceptance criteria · Effort/risk · Files & seams · Resume bullet.

**Current project state (so you don't re-propose finished work):** the interpreter + LLVM ORC
JIT (single-signal, fused, AVX2 cross-symbol vectorized, multi-symbol SoA, multi-thread),
tiered JIT (baseline + warm specialization), persistent module cache, SPSC ingest pipeline,
and recorded-data backtest + pandas oracle all exist and are parity-tested. **Stateful-operator
IR lowering is complete** — per `bench/results/lowering_gap_phase3/phase3_conclusion.md` the
fused JIT is ~**1.42×** of hand-written C++, so further raw-speed tuning has low marginal value.
The operator set includes stateless market/arithmetic ops, windowed ops (`sma`, `lag`,
`rolling_std/min/max`, `zscore`, `vwap`), recurrences (`ema`, `kalman1d`), pair ops
(`rolling_corr`, `rolling_beta`), and threshold ops (`cross_above/below`).

---

## ★ 1. Reverse-mode automatic differentiation over the signal IR  — **RECOMMENDED (most technically impressive)**

**What it is.** An autodiff system implemented as a *compiler pass over the signal IR*: given a
signal program, construct the **adjoint (backward) program** as new IR and JIT-compile it with
the existing LLVM pipeline, producing a native function that returns gradients of an output
(e.g. a loss/objective) with respect to the program's continuous parameters. Then use those
gradients to **calibrate signal parameters by gradient descent**.

**Why it's impressive.** This is differentiable-programming-compiler work, not "call PyTorch."
The rare, hard core is **differentiating through the recurrent/stateful operators** — `ema`
(`e_t = α·x_t + (1−α)·e_{t−1}`), `kalman1d` (predict/update recurrence), and Welford-based
`rolling_std` — whose adjoints must propagate **backward through time** (the same structure as
backprop-through-time), and you are *generating and compiling* that backward pass, not
interpreting a tape. Very few undergraduates build IR-level reverse-mode AD over a JIT. It is
legible to ML, quant (parameter sensitivities / calibration), and compiler audiences alike.

**Build-on (current state).** You already have: a typed IR/AST with stable `node_id`s, a
whole-program builder (`signal_program`), an LLVM IR emitter (`jit_compiler`), and the
interpreter as a reference. AD adds a new pass that consumes the same AST/IR and emits a second
function; it reuses the entire existing compile/JIT/runtime stack.

**Scope.**
1. Define the differentiable subset and parameter model. Continuous parameters (e.g. EMA `α`,
   thresholds, weights, blend coefficients) are differentiable; integer `period`s are **not**
   (document them as fixed, or relax via a smoothed surrogate only if explicitly chosen).
2. Implement local (per-op) VJPs for stateless ops (`+ − × ÷`, `abs`, `sqrt`, `log`,
   comparisons, `select`) and linear windowed ops (`sma`, `lag`, `vwap`: adjoint is a scatter
   over the window).
3. Implement the **recurrent adjoints** (`ema`, `kalman1d`) and the nonlinear windowed adjoint
   (`rolling_std`, `zscore` via chain rule). This is the crux.
4. Define behavior for non-smooth ops: `rolling_min/max` use subgradients (route to the
   arg-extremum); `cross_above/below` are non-differentiable thresholds → stop-gradient (or an
   optional smooth/straight-through surrogate, only if explicitly requested).
5. Emit the adjoint program as IR and JIT-compile it (reuse the existing pipeline + module cache).
6. Build a small **calibration driver**: pick an objective (e.g. maximize IC on recorded data,
   or fit to a target series), run gradient descent (e.g. Adam) over the continuous parameters,
   and report the optimized parameters + objective trajectory.

**Hard parts.** Backward-through-time accumulation for recurrences; correct adjoint of the
Welford rolling-std update; handling shared subexpressions (an IR node feeding multiple
consumers must *sum* incoming adjoints); keeping the backward pass allocation-free on the hot
path; deciding the parameter/data boundary (gradients w.r.t. parameters, not w.r.t. every tick).

**Validation (parity ethos).** Central finite-difference **gradient checking** for every
operator and for whole programs (the AD gradient must match `(f(θ+h) − f(θ−h)) / 2h` within
tolerance) — this is the AD analogue of your interpreter/JIT parity tests. Optionally cross-check
a re-implemented reference signal against **PyTorch autograd** as a second oracle (consistent
with your existing pandas-oracle methodology).

**Acceptance criteria.** (a) A `gradient_parity_test` that gradient-checks each differentiable
op and ≥3 whole programs (including one with `ema` and one with `kalman1d`) within documented
tolerance; (b) a compiled adjoint function (not an interpreted tape); (c) a calibration run on
recorded data that measurably improves the chosen objective vs the initial parameters, with a
reproducible artifact under `bench/results/`; (d) all existing suites still green.

**Effort/risk.** ~3–4 weeks; medium-high risk (the recurrence adjoints + FP-correct gradient
checks are where it can stall). Highest intellectual payoff on this list.

**Files & seams.** New: `src/autodiff.{h,cpp}` (adjoint construction), `cli/` or
`examples/calibrate_*` driver, `test/gradient_parity_test.cpp`. Touch: `signal_program`
(IR traversal), `jit_compiler` (emit + compile the adjoint), `runtime` (gradient state arena),
`interpreter` only as a value oracle (do **not** change its semantics).

**Resume bullet (numbers TBD by building it).**
> Implemented reverse-mode automatic differentiation as a compiler pass over the signal IR —
> constructing and JIT-compiling the adjoint of recurrent operators (EMA, Kalman, rolling
> statistics) — and used the compiled gradients to calibrate signal parameters by gradient
> descent, validated against finite-difference gradient checks.

---

## 2. Equality-saturation (e-graph) superoptimizer for the signal IR

**What it is.** Replace/augment ad-hoc optimization with an **e-graph + equality saturation**
optimizer: represent the signal IR in an e-graph, apply a rule set of semantics-preserving
rewrites (algebraic identities, strength reduction, cross-signal CSE, rolling-window identities)
to saturation, then **extract** the minimal-cost equivalent program under a cost model.

**Why it's impressive.** Equality saturation (egg / egglog, POPL'21; used in Cranelift, tensat)
is current compiler-research technology. Applying it to a domain with rich algebraic structure
and heavy shared subexpressions is a genuine optimizer, not a peephole hack.

**Build-on.** Your programs already exhibit shared subexpressions (existing fused-program load
dedup, 22→2). You have a canonical AST string (`AstCanonicalString`) and a clean IR to lower
from. Rust is on your stack, so `egg` is an option (or hand-roll the e-graph in C++ for more
credit).

**Scope.** Define the IR ⇄ e-graph translation; author rewrite rules with FP-safety side
conditions (no reorderings that violate the parity tolerances); define a cost model
(op latency / count); implement extraction; feed the extracted program into the existing JIT.

**Hard parts.** FP-safe rewrite rules (don't break parity via reassociation); a cost model that
reflects *measured* costs; proving the optimizer actually finds wins your existing O2 + fusion
miss (otherwise it's machinery for its own sake).

**Validation.** Parity of optimized vs unoptimized program (existing fuzz/parity harness);
an artifact showing concrete programs the e-graph improves (IR diff + benchmark delta).

**Acceptance criteria.** Rewrites are parity-preserving on the fuzz corpus; ≥1 program class
where the e-graph beats the current pipeline with a measured pinned-host delta; documented rule
set + cost model.

**Effort/risk.** ~3–4 weeks; medium-high (impressiveness hinges on demonstrable wins).

**Files & seams.** New `src/egraph_opt.*` (or a Rust crate + FFI), `test/egraph_parity_test.cpp`;
touch `signal_program`/`jit_compiler` at the optimize-before-lower boundary.

**Resume bullet.**
> Built an equality-saturation (e-graph) optimizer for the signal IR with floating-point-safe
> rewrite rules and a measured cost model, automatically discovering program rewrites beyond
> LLVM's O2 + my fusion pass, parity-verified against the interpreter.

---

## 3. SMT-based translation validation of JIT lowering (Alive2-style)

**What it is.** **Formally prove** that each JIT-lowered operator's emitted IR is semantically
equivalent to its reference semantics, by encoding both into an SMT solver (Z3) and checking
equivalence — translation validation, the technique behind Alive2 for LLVM.

**Why it's impressive.** It elevates the project's correctness story from "I fuzz it" to "I
prove it." Formal methods are rare and respected, and this is the most *on-brand* option for a
parity-obsessed codebase.

**Build-on.** You already maintain per-operator parity (`stateful_lowering_parity_test`) and a
differential oracle — translation validation is the formal capstone of that ethos.

**Scope.** Encode operator reference semantics and emitted IR into Z3; use the FP theory for
floating-point ops; bound-unroll recurrences/windows to a fixed horizon; check equivalence (or
bounded equivalence) and surface counterexamples.

**Hard parts.** **Floating-point semantics in SMT is the wall** — full FP reasoning is heavy and
can blow up. Scope realistically: bounded windows, possibly equivalence under real-arithmetic or
fixed FP rounding models, and be explicit about what is proved vs bounded-checked.

**Validation.** The tool *is* the validation; meta-test it by injecting a deliberate lowering
bug and confirming the checker catches it (and produces a counterexample).

**Acceptance criteria.** Checker proves (or bounded-proves) equivalence for ≥4 lowered ops;
catches an injected bug with a counterexample; documents the soundness scope (what's proved vs
bounded).

**Effort/risk.** ~3–4 weeks; high risk (FP-in-SMT can stall a solo effort). Most elegant fit.

**Files & seams.** New `src/translation_validation.*` (Z3 bindings), `test/tv_*`; reads
`jit_compiler` lowering + the reference op semantics.

**Resume bullet.**
> Built an SMT-based translation validator that formally proves my JIT-lowered operator IR is
> equivalent to the reference semantics (Alive2-style), catching lowering bugs with concrete
> counterexamples.

---

## 4. DSL → real language (user-defined functions, modules, cross-function inlining)

**What it is.** Extend the DSL from a fixed expression evaluator into a composable language:
named user-defined functions / signal definitions that call each other, optional imports/modules,
compiled by **inlining across function boundaries** into the same fused per-tick native function.

**Why it's impressive.** It's the purest "I built a compiler, not a calculator" signal: name
resolution/scoping, a real frontend, and cross-function inlining into the existing fused codegen.

**Build-on.** You have a lexer/parser, a **whole-program inliner already** (`AllocateProgramNodeIds`
structural dedup + `stateful_emit_cache`), and a scoped design doc (`docs/dsl_real_language.md`).
This is largely "extend the inliner to named callables + add a frontend for definitions."

**Scope.** Grammar for function/signal definitions and calls; name resolution + scoping +
arity/type checks; inline calls at IR-build time (reuse the inliner and node-id allocation);
keep constant-folding/formatter/lint working on the extended grammar.

**Hard parts.** Scoping + recursion policy (likely disallow unbounded recursion); ensuring
stateful operators inside reused functions get **distinct state slots per call site** (this is
exactly the cross-signal-aliasing class of bug your oracle already caught — guard it hard).

**Validation.** Parity of an inlined multi-function program vs the hand-expanded equivalent;
extend `fuzz_parity_test` to emit programs with user-defined functions.

**Acceptance criteria.** Function defs/calls parse, type-check, and compile via inlining;
per-call-site state isolation proven by a regression test; fuzz/parity green on programs using
UDFs; formatter/lint round-trip preserved.

**Effort/risk.** ~1–2 weeks; medium (state-isolation is the main hazard).

**Files & seams.** `src/lexer.*`, `src/parser.*`, `src/signal_program.*` (inliner + node ids),
`src/interpreter.*` + `src/jit_compiler.*` (must agree), `test/` parity + a UDF fuzz case.

**Resume bullet.**
> Extended the trading DSL into a composable language with user-defined functions and imports,
> compiled via cross-function inlining into a single fused per-tick native function, with
> per-call-site state isolation regression-tested.

---

## 5. Live + hot-reloadable real-time engine

**What it is.** Turn the batch/replay engine into a service that ingests a **real exchange feed**
(e.g. a crypto WebSocket) over the lock-free SPSC ring, and lets an operator edit a `.sig`,
recompile, and **atomically hot-swap** the new native function into the running pipeline with
**zero dropped ticks** and bounded latency.

**Why it's impressive.** The hot-swap-under-load is a real-time-systems / lock-free-concurrency
flex (RCU/atomic-pointer-swap of a live compiled function while ticks flow). It adds the one
dimension the codebase lacks: *it runs live*.

**Build-on.** You have the SPSC ring (`src/spsc_ring.h`, `spsc_ring_test`), the tiered-JIT
**atomic fn swap** (`TieredProgramJit`), and the ingest pipeline bench (`spsc_jit_pipeline_bench`,
~228 ns p50). This composes existing primitives.

**Scope.** A feed adapter (WebSocket client → normalize to `MarketState`); a control path to
recompile a signal and publish the new function pointer; an atomic swap protocol with safe
handling of in-flight state; end-to-end latency measurement.

**Hard parts.** Memory ordering of the swap; what happens to per-node state across a swap
(warm-state migration vs reset); not dropping or double-processing ticks at the boundary.

**Validation.** A test that performs N hot-swaps under a synthetic feed and asserts zero dropped
ticks and output continuity; latency histogram before/after swap.

**Acceptance criteria.** Live ingest from a real feed; ≥1 hot-swap under load with zero dropped
ticks proven by test; end-to-end latency artifact.

**Effort/risk.** ~1–2 weeks; medium.

**Files & seams.** New feed adapter + service `cli/`; `src/spsc_ring.h`, `TieredProgramJit`
(swap), `runtime` (state migration); `test/` swap-under-load test.

**Resume bullet.**
> Built a live signal-evaluation service ingesting a real exchange feed over a lock-free SPSC
> ring, hot-swapping recompiled signals into the running pipeline atomically with zero dropped
> ticks, at ~XXX ns p50 end-to-end.

---

## 6. Tail-latency hardening with a real p99.9 on isolated cores

**What it is.** Produce a rigorous, HFT-grade latency distribution (p99/p99.9/p99.99) under
coordinated-omission-corrected load on a **properly isolated Linux host** — not the current
WSL2-noise-dominated tails.

**Why it's impressive.** Tail latency under load is *the* thing low-latency-systems interviews
probe; a real, reproducible p99.9 on isolated cores is a strong, specific claim.

**Build-on.** You already have `latency_bench`, CO-aware pacing, and per-call histograms
(`bench/results/latency/`). The gap is the **environment**: `bench/PINNED_HOST.md` is WSL2,
which can't `isolcpus`/`nohz_full` or lock turbo.

**Scope.** Stand up a bare-metal/tuned Linux host (dual-boot or cloud bare-metal): `isolcpus`,
`nohz_full`, disabled SMT/turbo wobble, `perf`-validated; integrate HdrHistogram; report a full
distribution with the methodology documented.

**Hard parts.** Mostly environmental/methodological; the engine is already alloc-free on the hot
path (`hot_path_allocation_test`).

**Validation.** Reproduce-from-clean instructions; show tails are stable (not scheduler noise).

**Acceptance criteria.** p99.9 (and p99.99) per signal on an isolated host with documented
kernel/CPU config and a reproduce script; honest CO correction.

**Effort/risk.** ~1 week (plus hardware access); low-medium. **Blocked without a non-WSL host.**

**Files & seams.** `bench/` (new pinned-host profile + driver), `latency_bench`; new
`bench/PINNED_HOST_baremetal.md`.

**Resume bullet.**
> Measured per-signal latency to p99.9 = XX ns under coordinated-omission-corrected load on
> isolated Linux cores (isolcpus/nohz_full, turbo-locked), with an allocation-free hot path
> verified by perf.

---

## 7. Second codegen backend: GPU/CUDA from the same IR

**What it is.** A second compiler backend that emits **CUDA kernels** (or PTX) from the same
front-end IR, evaluating many symbols × signals massively in parallel on the GPU, with output
parity against the CPU JIT.

**Why it's impressive.** A second target proves the frontend/IR is properly decoupled from the
backend — real compiler architecture. (Note: throughput story, not latency; and your NN-compiler
project already shows CUDA codegen, so this partially overlaps existing evidence.)

**Build-on.** The multi-symbol SoA layout (`MultiSymbolSignalContext`) maps naturally to GPU
threads (one symbol per thread/lane); the IR is already backend-agnostic enough to retarget.

**Scope.** IR → CUDA C (or PTX) emitter; SoA device memory layout + transfers; port operator
semantics (incl. per-thread stateful state); parity vs CPU JIT; a throughput benchmark vs the
multi-thread CPU path at large symbol counts.

**Hard parts.** Stateful operators per thread (state in global/shared memory), warp divergence
on threshold ops, transfer overhead amortization, FP parity with the CPU path.

**Validation.** Bit-or-tolerance parity of GPU vs CPU JIT on the multi-symbol fuzz corpus.

**Acceptance criteria.** GPU backend matches CPU JIT within tolerance on the fuzz corpus; a
throughput crossover artifact (GPU vs CPU multi-thread) at large N.

**Effort/risk.** ~3–4 weeks; higher (CUDA correctness + parity). Highest "wow", lowest novelty
relative to your existing portfolio.

**Files & seams.** New `src/cuda_backend.*` + `.cu` codegen, device runtime; `test/` GPU-vs-CPU
parity; `bench/` GPU throughput.

**Resume bullet.**
> Retargeted the compiler to emit CUDA kernels from the same front-end IR, evaluating millions of
> symbol×signal pairs per tick on the GPU with output parity against the CPU JIT.

---

## 8. Speculative tiered JIT with deoptimization / on-stack replacement

**What it is.** Extend the existing tiered JIT into a true **speculative** compiler: specialize a
hot program on profiled invariants (beyond `assume_warm`), guard the speculation, and provide a
**deopt path** that falls back to the generic version when a guard fails.

**Why it's impressive.** Deopt/OSR is exactly what V8 TurboFan, LuaJIT, and HotSpot do — serious
VM-implementation territory.

**Build-on.** You already have `TieredProgramJit` (baseline + warm specialization), atomic fn
swap, `JitProfile{assume_warm}`, and `ComputeProgramWarmupThreshold`. The deopt path is the
missing piece.

**Scope.** Identify additional speculatable invariants (e.g. constant market parameters, stable
flags); emit guards + a deopt/bailout to the generic function; ensure state consistency across a
deopt.

**Hard parts.** **Motivation is the weak point** — signal inputs are well-behaved, so find a
*genuine* invariant worth speculating on, or this reads as machinery without payoff. State
consistency across deopt is the correctness hazard.

**Validation.** A test that forces a guard violation and asserts correct deopt + identical output
to the generic path.

**Acceptance criteria.** ≥1 real speculated invariant with a measured pinned-host win; a
forced-deopt test proving correctness; parity preserved.

**Effort/risk.** ~2–3 weeks; medium-high, with motivation risk.

**Files & seams.** `src/jit_compiler.*` (`TieredProgramJit`, `JitProfile`), `runtime` (deopt
state), `test/tiered_specialization_parity_test.cpp` (extend).

**Resume bullet.**
> Extended the tiered JIT with profile-guided value speculation and a deoptimization path
> (V8/LuaJIT-style), bailing safely to the generic compilation when a speculated invariant is
> violated, with correctness gated by forced-deopt tests.

---

## Quick ranking (for a chooser)

| # | Option | Technical impressiveness | Effort | Risk | Notes |
|---|---|---|---|---|---|
| **1** | **Autodiff over the IR** | **Highest** | 3–4 wk | Med-high | Recommended; hard *ideas*, defensible, useful |
| 2 | E-graph superoptimizer | High | 3–4 wk | Med-high | Current research tech; needs demonstrable wins |
| 3 | SMT translation validation | High | 3–4 wk | High | Most on-brand; FP-in-SMT is the wall |
| 4 | DSL → real language | Med-high | 1–2 wk | Med | Pure compiler cred; cheapest of the deep ones |
| 5 | Live + hot-reload | Med-high (systems) | 1–2 wk | Med | Adds the "runs live" dimension |
| 6 | Tail-latency rigor | Med (HFT-specific) | ~1 wk | Low-med | Blocked without a non-WSL host |
| 7 | GPU/CUDA backend | High "wow", lower novelty | 3–4 wk | Higher | Overlaps your NN-compiler CUDA work |
| 8 | Speculative/deopt JIT | High (VM tech) | 2–3 wk | Med-high | Weak motivation for this domain |
