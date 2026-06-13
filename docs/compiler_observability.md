# P9 — Compiler observability

A real compiler engineer obsesses over what their compiler produced.
P9 adds the pieces that let any reviewer (or future-you) inspect what
the JIT actually emits for any given DSL program and assert specific
properties about the output.

Three parts:

1. **CLI dump flags** on `jit_signal_engine`: `--dump-ast`,
   `--dump-ir-pre-opt`, `--dump-ir-post-opt`, `--dump-asm`.
2. **A checked-in `llvm-mca` analysis** of the fused
   `filtered_momentum` program, with per-instruction port pressure
   and predicted IPC.
3. **A test that asserts specific optimization outcomes** in the
   post-O2 IR and the captured asm.

---

## CLI dump flags

| Flag                       | Alias            | Output                                                                |
|----------------------------|------------------|------------------------------------------------------------------------|
| `--dump-ast`               | `--print-ast`    | The parsed AST for either the named signal or every signal in the file (with `--all-signals`). |
| `--dump-ir-pre-opt`        | `--dump-ir-pre`  | LLVM IR for the JIT-emitted module, captured *before* the O2 pass pipeline runs. |
| `--dump-ir-post-opt`       | `--dump-ir`      | LLVM IR for the same module, captured *after* the O2 pass pipeline runs. |
| `--dump-asm`               | -                | Host-target x86-64 assembly for the post-O2 module, produced by `TargetMachine::addPassesToEmitFile(AssemblyFile)`. |

Example usage:

```bash
./build/jit_signal_engine --dump-asm --all-signals \
    examples/filtered_momentum.sig 100 2>filtered_momentum.s 1>/dev/null
head -30 filtered_momentum.s
```

```
        .text
        .file   "jit_signal_program_module"
        .section        .rodata.cst8,"aM",@progbits,8
        .p2align        3, 0x0
.LCPI0_0:
        .quad   0x3fe0000000000000
.LCPI0_1:
        .quad   0x3fc745d1745d1746
        ...
        .globl  signal_program_func_1
signal_program_func_1:
        .cfi_startproc
        pushq   %r15
        ...
```

The dump flags print to **stderr** so they can be redirected
independently of the benchmark output on stdout. This mirrors the
LLVM tool convention.

### How the asm dump works

`JitCompiler::Compile*` already captured the pre- and post-O2 IR by
calling `module->print(raw_string_ostream)`. P9 adds a third capture
in the same scope:

```c++
{
  std::string asm_err;
  impl.last_asm = EmitHostAsm(*module, asm_err);
}
```

`EmitHostAsm` clones the module, builds a fresh `TargetMachine`
matching the host triple but with an *empty features string* (so the
dump shows baseline-ISA codegen, which is what `llvm-mca` defaults to
and what we want as a reproducible artifact), then runs the legacy
`PassManager` with `CGFT_AssemblyFile` to capture asm into a
`SmallVector`. The legacy PM is used because the new pass manager
does not yet expose a public asm-emit entry point.

The captured asm is the post-O2 module compiled for the **baseline
ISA**, not necessarily the exact code ORC ends up loading (ORC may
use host features such as AVX2 if the LLJIT TargetMachine was
configured with them). The two will be functionally identical but
may differ in vector width / specific instruction choice. The
trade-off is deliberate: the asm dump's value is reviewability and
reproducibility, not byte-for-byte fidelity to the executed code.

---

## llvm-mca artifact

`bench/llvm_mca_filtered_momentum.sh [cpu]` dumps the host asm for
`filtered_momentum` and runs it through `llvm-mca` to produce a
predicted scheduling report. The output is checked in at
`bench/results/llvm_mca/filtered_momentum.txt`.

A snapshot from a Skylake-modeled run (top of the report):

```
Iterations:        100
Instructions:      10400
Total Cycles:      7876
Total uOps:        16500

Dispatch Width:    6
uOps Per Cycle:    2.09
IPC:               1.32
Block RThroughput: 27.5
```

What this tells a reviewer:

* **IPC 1.32** is a healthy steady-state IPC for code dominated by
  scalar-double FP arithmetic with serial data dependencies (each
  rolling op feeds the next). It is *not* a vectorized number and we
  don't claim it is; the JIT path is scalar by design.
* **uOps/cycle 2.09** vs **Dispatch Width 6**: the front-end is not
  saturated, meaning the bottleneck is the dependency chain through
  the stateful rolling-op runtime calls (each `jit_rt_*` is modeled
  by llvm-mca as a 100-cycle latency placeholder, which dominates
  every other latency in the block).
* The **`call`-instruction warning** in the asm output makes the
  bottleneck explicit: `llvm-mca` warns that it cannot model call
  instructions, which is the same architectural point we make
  elsewhere — the stateful runtime helpers are opaque to LLVM.

The report's detailed per-instruction section lets you see exactly
which instructions go to which execution ports and predicts where
back-end pressure accumulates. The full file is 255 lines.

### Why predicted, not measured

`llvm-mca` is a static analyzer; it doesn't run the code. The
predictions are useful as evidence of *what the compiler emitted* —
"the compiler produced code that, according to a published cycle
model, has IPC 1.32 on Skylake". We do not claim 1.32 is the
measured IPC of the running JIT; for that, see the bootstrap-CI
pinned-host benchmarking in `bench/PINNED_HOST.md`.

To regenerate the artifact after a code change:

```bash
bench/llvm_mca_filtered_momentum.sh skylake   # or any -mcpu= LLVM supports
```

---

## Optimization-evidence test

`test/optimization_evidence_test.cpp` is a unit test that asserts
specific properties of the post-O2 IR and the captured asm. It is
the regression gate for "the optimizer is doing the work we want".

Three checks:

1. **Opaque path (`kNone`) keeps runtime calls.**
   Compile `filtered_momentum.sig` with `StatefulLoweringFlags::kNone`
   (production default is `kAll`; this exercises the legacy opaque path)
   and assert the post-O2 IR contains at least one `@jit_rt_ema*`
   call. If it doesn't, the runtime helper path has silently broken
   and the linker would fail at ORC-lookup time on a future call
   site.

2. **Full lowering (`kAll`, production default) eliminates runtime calls for lowered ops.**
   Compile the same program with `StatefulLoweringFlags::kAll` and
   assert the post-O2 IR contains exactly **zero** matches of
   `@jit_rt_sma*`, `@jit_rt_ema*`, `@jit_rt_lag` (and, after Phase 2,
   the other lowered stateful helpers). Inline lowering replaces these
   calls with IR; this test enforces that the replacement happened.

3. **Asm dump is non-empty and contains real FP arithmetic.**
   Both the default-lowered and full-lowered asm strings are required
   to be non-empty and to contain at least one of the x86 double-
   precision FP mnemonics `mulsd|addsd|subsd|divsd|vfmadd*|vmulsd|
   vaddsd|vsubsd|vdivsd`. This catches the case where `EmitHostAsm`
   silently fails (e.g., target backend not registered) and returns
   an empty string.

A passing run prints:

```
default_rt_sma=0
default_rt_ema=9
default_rt_lag=0
full_rt_sma=0
full_rt_ema=0
full_rt_lag=0
default_asm_bytes=2937
full_asm_bytes=3630
default_total_rt_calls=11
optimization_evidence_test=pass
```

The `default_rt_ema=9` count tells you the IR contains 9 separate
`jit_rt_ema_alpha` call sites (one per ema appearance after
inlining). Under full lowering all 9 disappear and the IR contains
zero ema calls. The asm-byte counts are not asserted exactly (they
will drift as the codebase evolves) — only that they are non-zero.

### What this test does *not* check

It does not assert absolute IPC, vectorization width, or specific
register usage. Those are too brittle across LLVM upgrades. The
invariants we test are the **structural** ones: "lowered ops produce
zero calls", "default mode produces calls", "asm is real FP code".
For the cycle-level analysis, see the `llvm-mca` artifact.

---

## Implementation notes

* `EmitHostAsm` lives in an anonymous namespace in `jit_compiler.cpp`
  and is `#ifdef JITSE_HAS_LLVM` so it compiles out cleanly when LLVM
  is unavailable.
* `buffer_ostream(raw_svector_ostream(buf))` is the dance required by
  `addPassesToEmitFile`, which needs a `pwrite`-capable stream;
  `raw_svector_ostream` does not provide `pwrite`. The
  `buffer_ostream` wrapper buffers writes and flushes on
  destruction, so it is confined to a scope and `buf.str()` is read
  *after* the scope ends.
* The three sites that capture post-O2 IR (single signal,
  `CompileProgram`, `CompileProgramVectorized`) each capture asm in
  the same scope. The single-signal site additionally surfaces any
  failure as `; asm dump unavailable: <reason>\n` in `last_asm` so a
  user running `--dump-asm` can see *why* their dump is empty.

## How to use this when reviewing the project

The fastest way for a new reviewer to understand what the JIT
actually produces:

```bash
# 1. See the AST for the headline example.
./build/jit_signal_engine --dump-ast --all-signals examples/filtered_momentum.sig 1 2>/dev/null

# 2. See the IR LLVM saw before O2 ran.
./build/jit_signal_engine --dump-ir-pre-opt --all-signals examples/filtered_momentum.sig 1 2>ir_pre.ll

# 3. See the IR after O2 finished.
./build/jit_signal_engine --dump-ir-post-opt --all-signals examples/filtered_momentum.sig 1 2>ir_post.ll

# 4. See the host asm the backend emitted.
./build/jit_signal_engine --dump-asm --all-signals examples/filtered_momentum.sig 1 2>filtered_momentum.s

# 5. See the cycle/port-level analysis.
less bench/results/llvm_mca/filtered_momentum.txt
```

Five commands, five layers of the compilation pipeline, all
inspectable from a single binary.
