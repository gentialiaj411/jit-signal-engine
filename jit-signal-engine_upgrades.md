# JIT-Signal-Engine — Elite Upgrade Plan

**Status:** **complete (P0–P2, 2026-05-24).** Follow-on work also landed: Welford `rolling_std`, `bench/results/avx2_speedup/` (vec vs scalar JIT), SPSC pipeline (`bench/results/spsc_pipeline/`), roadmap P0–P15, and **operator lowering Phases 0–4** (default `kAll`, fused JIT÷hw **1.42×**, P4 lowered+vec parity). Keep this file as the historical plan; use `NEXT_TASK.md` for optional follow-ons. Read alongside `CLAIMS_MATRIX.md`, `RESUME_CLAIMS.md`, `EVIDENCE.md`, `ARCHITECTURE.md`, and `PROJECT_ROADMAP.md`.

**Rule for this plan:** every task is justified by exactly one outcome — *it changes a resume bullet*. Tasks that only harden internals, add CI gates, or produce paper writeups have been removed. If a task does not move a public-facing claim, it does not belong here.

**Audience:** an LLM agent doing scoped, evidence-backed implementation work in this repo.

---

## Read first (mandatory)

In this order, before any code change:

1. `AGENTS.md` and `CLAUDE.md` — read order, exploration discipline, reasoning rules, build/test/bench commands, documentation/resume rules. These bind.
2. `PROJECT_CONTEXT.md`, `PROJECT_STATE.md`, `PROJECT_ROADMAP.md` — current snapshot of phase status, what is verified vs. supported, and roadmap intent.
3. `CLAIMS_MATRIX.md` and `EVIDENCE.md` — the source of truth for what claims this project will defend. Every new claim lands here.
4. `RESUME_CLAIMS.md` — new resume-ready bullets only after the matching `CLAIMS_MATRIX.md` row is `Verified`.
5. `bench/results/diff_test/divergence_report.md` and `bench/results/cse_evidence/cse_diff_verified.md` — correctness oracle and verified load-dedup evidence.

If a task below conflicts with `AGENTS.md` or `CLAUDE.md`, those win. Stop and ask.

---

## Operating rules

1. **No performance claim without a pinned-host artifact.** All speedup numbers must be produced on a documented, isolated, repeated benchmark configuration. One-off numbers are not claims.
2. **The interpreter is the ground truth.** Every JIT correctness claim is "JIT vs. interpreter parity on fuzz / recorded data." Do not regress this.
3. **Existing claim discipline binds.** A claim that says `Supported` in `CLAIMS_MATRIX.md` cannot be promoted to `Verified` without new evidence — and the existing "fix or drop" rule for CSE/load-dedup applies here too.
4. **Honest decreases beat dishonest increases.** If a re-run produces a smaller speedup than the historical artifact, the new number wins. Period.
5. **Every task names the bullet it changes.** If you cannot, the task is out of scope and must not be done as part of this plan.

---

## Tasks, prioritized

### P0 — Re-establish a reproducible JIT-vs-interpreter speedup on a pinned host

**Resume bullet this changes:** the current B1 — *"Built a JIT compiler in C++ ... achieving 10.3x higher throughput over an equivalent tree-walking interpreter"* — has its `10.3x` number replaced with a pinned-host number that survives a clean rerun. The historical `10.3x` / `13x` numbers from a different host are demoted, not erased.

**Why first:** until this is reconciled, every public bullet on this project is fragile. The latest re-run shows `4.34x` (momentum) and `5.43x` (all-signals fused) — the current resume's `10.3x` and `13x` do not survive a fresh checkout, which means an interviewer who reruns the bench will catch you.

**Goal:** a documented, reproducible, "this is the canonical machine and config" benchmark that produces a number we are willing to defend.

**Sub-tasks**

1. **Pick the canonical host.** Document under `bench/PINNED_HOST.md`: OS (WSL2 / bare Linux / Windows), CPU model, frequency governor setting, isolated cores, hugepage configuration, OS scheduler config, exact compiler version, LLVM version.
2. **Pin the benchmark harness.** Modify the existing harness to:
   - Use `taskset` / core-pinning on the documented cores.
   - Verify governor is `performance` before running; fail loudly otherwise.
   - Repeat each measurement N ≥ 30 times and report median, p99, and 95% bootstrap confidence interval.
   - Refuse to run if hyperthreading siblings of pinned cores are active.
3. **Run two benchmarks on the pinned host:**
   - Momentum single-signal: interpreter vs. JIT throughput.
   - All-signals-fused: interpreter vs. JIT throughput.
4. **Land `bench/results/pinned_host_speedup.json`** with the raw data and `bench/results/pinned_host_speedup.md` with the headline numbers and confidence intervals.
5. **Update `CLAIMS_MATRIX.md`:** replace the historical 10.x/13.x numbers with the pinned-host numbers. Note the historical artifact under a clearly-labeled "historical, different host" row, not a `Verified` row.
6. **Update `RESUME_CLAIMS.md`** with the new pinned-host number wired into B1.

**Acceptance**

- `bench/PINNED_HOST.md` exists and is concrete (no "depends on machine" hand-waves).
- Pinned harness lands and refuses to run outside the pinned configuration.
- One JSON + one MD artifact under `bench/results/`.
- `CLAIMS_MATRIX.md` and `RESUME_CLAIMS.md` now reference the pinned numbers.
- Historical claims are not silently deleted; they are demoted with a "historical, different host" label.

**Non-goals**

- Improving the JIT to recover the historical 10x/13x. Measure honestly first.
- Adding new backends.

---

### P1 — Resolve the CSE / load-dedup claim once and for all

**Resume bullet this changes:** the current B2 — *"Sped up multi-signal evaluation 13x over an interpreted baseline by compiling all dependent signals into one native function per tick, letting the compiler eliminate redundant market data reads across signals"* — has its "eliminate redundant market data reads" clause either *kept and verified*, or *removed* in favor of the narrower verified wording: "compiling all dependent signals into one native function per tick."

**Why:** `CLAIMS_MATRIX.md` flags this as `Supported`, not `Verified`. The IR diff in `bench/results/cse_evidence/` shows market-data load counts stayed `22 → 22`. The current resume bullet implies a compiler behavior that is not actually verified to occur — this gets caught on a 30-minute deep-dive interview.

**Goal:** either (a) the compiler actually deduplicates redundant loads and the IR-diff test proves it, or (b) the claim is removed from `RESUME_CLAIMS.md` and `README.md`.

**Sub-tasks**

1. **Investigate why CSE/load-dedup is not firing.** Read the lowering path; check whether aliasing metadata is missing, whether loads are marked `volatile`, whether scoped noalias / TBAA is absent, whether `-O2`/`-O3` is being applied at all to the JIT module.
2. **If a fix is feasible without rearchitecting:**
   - Apply it.
   - Extend the IR-diff harness to count loads pre- and post-CSE and assert a strict reduction on a documented signal program.
   - Land `bench/results/cse_evidence/cse_diff_verified.md` with the new counts.
   - Promote the matrix row to `Verified`.
   - Keep the "eliminate redundant market data reads" wording in B2.
3. **If a fix is not feasible inside this task's budget:**
   - Mark the matrix row `Not supported — drop from public claims`.
   - Remove the "eliminate redundant market data reads" clause from `RESUME_CLAIMS.md` and any equivalent line in `README.md`.
   - Replace B2 with the narrower verified wording: *"Compiled all dependent signals into one native function per tick, evaluated end-to-end against an interpreter baseline with fuzz parity."*

**Acceptance**

- Exactly one of the two outcomes above is realized. No middle state.
- The matrix row is now either `Verified` (with evidence) or `Not supported` (with downstream wording fixed in `RESUME_CLAIMS.md` and `README.md`).
- `bench/results/cse_evidence/` reflects the final state.

**Non-goals**

- Adding new optimizations beyond CSE/load-dedup. Surgical scope.
- Touching the interpreter.

---

### P2 — Multi-threaded multi-symbol execution scaling

**Resume bullet this changes:** enables a brand-new bullet — *"Scaled multi-symbol signal evaluation to X symbols across N CPU cores at Y% scaling efficiency on a pinned host, by partitioning the symbol set across worker threads operating on a single compiled program."* This is a clean, distinct performance axis from the JIT-vs-interpreter story (a different number, a different graph, a different question in interviews) and does **not** overlap with the uTPU compiler/accelerator narrative.

**Why:** the engine currently has exactly one performance axis recruiters can probe (single-thread JIT vs. interpreter). Adding *measured multi-core scaling* produces a new resume bullet on an axis HFT-adjacent recruiters expect a signal engine to demonstrate, and it pairs naturally with the existing multi-symbol SoA execution and AVX2 SIMD work without requiring any architecture overhaul.

**Goal:** the existing multi-symbol SoA execution path, when run across N worker threads on a pinned host, produces a documented scaling factor and bit-identical output to the single-threaded baseline.

**Sub-tasks**

1. Audit the existing multi-symbol SoA execution path in `src/`. Identify the smallest change to partition symbols across worker threads (each thread owns a contiguous shard of the symbol set, shares the compiled program, holds its own per-thread rolling-state arrays).
2. Implement thread-per-shard partitioning behind a `--threads N` flag. The existing single-threaded path must remain the default until the new path is `Verified`.
3. Per-thread state arrays only. **No lock-free shared rolling state.** Each thread reads the same market-state snapshot for the tick (immutable during the tick), writes only into its own per-thread state slabs.
4. NUMA-aware allocation on the pinned host if it has multiple NUMA nodes (via `numactl` / `libnuma`); otherwise skip and document the choice.
5. **Output equivalence test:** across the full multi-symbol benchmark, the concatenated multi-threaded output must hash-equal the single-threaded output (within float-exact bitwise equality, since per-symbol rolling state is independent). Land this as a test.
6. **Sustained-throughput benchmark** at `{1, 2, 4, 8, 16}` threads on the pinned host (re-using the `bench/PINNED_HOST.md` config from P0). Record symbols/sec, p99 per-tick wall-clock latency, and per-thread CPU utilization. Run ≥ 60 seconds per thread count.
7. Land `bench/results/multithread_scaling.json` (raw) and `bench/results/multithread_scaling.md` (headline scaling factor per thread count, pinned-host config reference).
8. **Update `CLAIMS_MATRIX.md`:** `Multi-threaded multi-symbol execution scales to N threads at X% efficiency on the pinned host, bit-identical output to single-threaded baseline`, status `Verified`. Record the *measured* X and N — do not promise linear.
9. **Update `RESUME_CLAIMS.md`:** add the new bullet with the measured numbers. If the user wants to maintain a 3-bullet count, this bullet is a candidate to replace whichever of the existing three currently sells the project shortest.

**Acceptance**

- Multi-threaded path lands behind a feature flag.
- Hash-equality test against the single-threaded path passes across the full benchmark.
- Scaling artifact landed with *measured* numbers (not extrapolated).
- New `Verified` row in `CLAIMS_MATRIX.md` and corresponding bullet drafted in `RESUME_CLAIMS.md`.

**Non-goals**

- GPU / CUDA. That is `uTPU`'s territory; do not encroach.
- Multi-process or distributed multi-symbol execution. Single-host threads only.
- Re-compiling per thread. One compiled program, many threads.
- Lock-free shared rolling state across threads. Per-thread state slabs only.
- Work-stealing scheduler. Static shard-per-thread partitioning is the scope.
- Touching the interpreter.

---

## Sequencing

P0 first. P1 can run in parallel with P0 (different files, different concerns). P2 after P0 lands — it depends on the pinned-host harness.

---

## Definition of done for the plan

- `CLAIMS_MATRIX.md` has 2 new `Verified` rows (P0, P2) and one settled CSE row from P1 (`Verified` or `Not supported`).
- `RESUME_CLAIMS.md` reflects only `Verified` claims, with:
  - B1's number replaced by the pinned-host number (from P0).
  - B2's "eliminate redundant reads" clause either verified or removed (from P1).
  - A new multi-threaded multi-symbol scaling bullet drafted (from P2).
- No bullet in `README.md` exceeds what `CLAIMS_MATRIX.md` says.
- All new `bench/results/` artifacts are reproducible from a clean checkout on the pinned host.

---

## Forbidden behaviors (specific to this plan)

- Do not promote any `Supported` row in `CLAIMS_MATRIX.md` to `Verified` without new evidence landing in `bench/results/`.
- Do not silently delete historical rows. Demote, do not erase.
- Do not introduce a GPU/CUDA backend in this repo. That is `uTPU`'s territory.
- Do not add a CUDA, OpenCL, SYCL, or any accelerator backend to lower the IR. CPU-only in this repo.
- Do not modify the sibling `market-data-handler` repo from this repo.
- Do not edit any LaTeX resume file. Resume drafts live outside this repo.
- Do not add new tasks to this plan that do not name the exact resume bullet they would change.
