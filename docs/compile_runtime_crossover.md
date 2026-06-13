# P5 — Compile-time vs runtime tradeoff measurement

## What this document is

A JIT's pitch is **"the per-event work is faster but you pay a one-shot
compile cost"**. P5 quantifies both halves of that pitch and computes
the breakeven point per signal program. The story now reads:

> "filtered_momentum's interpreter does 229 ns/event, the JIT does 101
> ns/event, and the JIT takes 3.34 ms to compile. So the JIT pays
> itself off at **N\* ≈ 26 000 events** (about 6 ms of warm processing
> at the JIT rate)."

That's a real systems insight — and importantly, it's a number, not a
hand-wave. The artifact lives at
[`bench/results/crossover/`](../bench/results/crossover/) (start with
[`index.md`](../bench/results/crossover/index.md)).

## What changed in the JIT code

[`src/jit_compiler.h`](../src/jit_compiler.h) gained a new public type:

```cpp
struct CompileTimings {
  std::uint64_t ast_to_ir_ns = 0;     // walk AST -> build llvm::Module
  std::uint64_t llvm_opt_ns = 0;      // buildPerModuleDefaultPipeline(O2).run
  std::uint64_t orc_codegen_ns = 0;   // addIRModule + lookup
  std::uint64_t total_ns = 0;
};

CompileTimings JitCompiler::LastCompileTimings() const;
```

`CompileProgramImpl` (the shared body for `CompileProgram` and
`CompileProgramSpecialized`) takes `steady_clock` timestamps at the
three natural phase boundaries and writes the breakdown into
`impl.last_compile_timings` on success. Failed compiles zero the
timings so the accessor is unambiguous. The timing is best-effort:
bookkeeping outside the timed scopes (a few µs at most) is the gap
between `ast_to_ir + llvm_opt + orc_codegen` and `total_ns`. The smoke
gate [`test/compile_timings_test.cpp`](../test/compile_timings_test.cpp)
checks the gap is within 5% or 50 µs of `total_ns`, whichever is larger.

`CompileProgramVectorized` and the single-signal `Compile` path do not
yet populate the timings — they're not exercised by this artifact. The
header documents the scope.

## Why three phases (and not finer)

These three are the right granularity for the question:

- **AST → IR** is the only "our code" cost. Optimizing this would
  reduce surface area in `jit_compiler.cpp`. It is ~5% of the budget;
  not worth chasing.
- **LLVM O2** is the optimization pipeline. Going from O0 to O2 is what
  buys us the per-event speedup; cutting it would speed up compile but
  also slow down the JIT'd code. The right tradeoff knob, if any, lives
  here.
- **ORC codegen** is machine-code emission. ISel + regalloc + memory
  write. Hard to avoid without changing JIT backend (e.g. switching to
  Cranelift would shrink this phase).

Finer breakdowns (per-pass timings, separate addIRModule vs lookup,
etc.) would require LLVM-internal hooks and aren't worth the noise for
the artifact's question.

## How the bench computes the crossover

[`bench/compile_runtime_crossover.cpp`](../bench/compile_runtime_crossover.cpp)
runs three measurement loops:

1. **Compile**: K (default 7) fresh `JitCompiler` instances each
   compile the same program. We report **min per phase** because all
   noise on this measurement is positive (scheduler interrupts, page
   fault on first compile, etc.). The reported `total_ns_best` is the
   smallest of the K runs. We also report `total_ns_median` so the
   reader can sanity-check noise level.
2. **Interpreter per-event**: 200 000 events, 5% warmup, closed loop.
   Best of K (default 5) runs. Returns ns/event.
3. **JIT per-event**: identical loop with the JIT function pointer.

Crossover formula:

```
N* = T_compile / (interp_ns_per_event − jit_ns_per_event)
```

A negative or infinite `N*` (no per-event win) returns "NONE" with a
pointer at [`docs/runtime_call_profile.md`](runtime_call_profile.md):
this typically only happens when the program is dominated by
non-lowered runtime helpers the JIT can't out-execute.

## Outputs per signal

- **CSV** (`{stem}_crossover.csv`): cumulative wall-time samples at log
  decades of N. Suitable for re-plotting in Python / R.
- **Markdown** (`{stem}_crossover.md`): the per-phase table, the
  per-event table, the breakeven formula with concrete numbers
  substituted in, and a plain-English sentence ("don't bother
  compiling unless …").
- **SVG** (`{stem}_crossover.svg`): self-contained log-log plot of
  cumulative interpreter time vs cumulative `T_compile + JIT` time.
  Crossover is marked with a green dashed line and dot. No external
  fonts / CSS / JS — embeddable anywhere.

## Limitations

- **First JIT compile in a process pays an LLVM init tax.** That tax
  is ~10 ms on this host and depends on linked LLVM build flags. The
  bench does an explicit warmup compile first so the reported
  `T_compile` is the *per-program* steady-state cost. If your
  production process does exactly one JIT compile and exits, add the
  warmup cost back.
- **Per-event timing is `steady_clock` granularity** (~10-50 ns
  overhead). For very fast signals like `spread_signal` (~2 ns/event in
  JIT mode) the clock cost is comparable to the work, which is why the
  reported numbers are slightly above the "pure" execution cost. The
  crossover formula is still correct because both interp and JIT pay
  the same clock cost.
- **WSL2 / non-isolated host.** Numbers are reproducible enough for the
  "best of K" pattern to work, but absolute values are not directly
  comparable to a properly-pinned bare-metal host. On a pinned host
  expect ~10-30% tighter per-event numbers; compile timings move less.
- **No O0/O1 sweep.** A natural extension would be "what's the
  crossover at O1?" — likely a much smaller `T_compile` with a
  larger `jit_ns_per_event`. The header's per-phase break-down already
  supports this experiment without further plumbing.
