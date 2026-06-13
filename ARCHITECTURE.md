# ARCHITECTURE

A reader's guide to the JIT signal engine: the pipeline from a DSL
source string to a vectorized, multi-threaded, on-disk-cached
machine-code function pointer. The `PROJECT_ROADMAP.md` describes the
individual P-tasks chronologically; this document ties them together
into one path.

If you only read one section, read [The Pipeline](#the-pipeline). The
rest is detail.

---

## The Pipeline

A signal program goes through these stages, in order. Each stage
hands a stricter representation to the next.

```
        +-----------------------------+
        |  DSL source (string)        |
        +-------------+---------------+
                      |   (lexer.cpp)
                      v
        +-------------+---------------+
        |  Tokens (with SourceLoc)    |
        +-------------+---------------+
                      |   (parser.cpp)
                      v
        +-------------+---------------+
        |  AST (Expr tree per signal) |
        +-------------+---------------+
                      |   (type_check.cpp)
                      |   (constant_fold.cpp)
                      v
        +-------------+---------------+
        |  Typed/folded AST           |
        +-------------+---------------+
                      |   (signal_program.cpp::InlineSignalDependencies)
                      v
        +-------------+---------------+
        |  Inlined program            |
        +-------------+---------------+
                      |   (signal_program.cpp::AllocateProgramNodeIds)
                      |    + P11 stateful-subtree dedup
                      v
        +-------------+---------------+
        |  Program with node_ids      |
        +-------------+---------------+
                      |
            +---------+---------+
            |                   |
            v                   v
       +----+----+        +-----+------+
       | Interp  |        | JIT (LLVM) |
       +---------+        +-----+------+
                                |
                +---------------+----------------+
                |                                |
                v                                v
          scalar compile                   vec compile (K lanes)
          (P1 + P3 + P9)                   (P2 + P10 fan-out)
                |                                |
                +---------------+----------------+
                                |
                                v
                       +--------+--------+
                       | LLVM PassMgr O2 |
                       +--------+--------+
                                |        (P13: bitcode persisted here on miss,
                                |         restored here on hit, skipping IR+O2)
                                v
                       +--------+--------+
                       |   ORC codegen   |
                       +--------+--------+
                                |
                                v
              +-----------------+----------------+
              | machine-code fn ptr (per signal) |
              +-----------------+----------------+
                                |
                                |   per tick, per symbol:
                                v
                +---------------+----------------+
                |  fn(market, arena, sym, out)   |
                +--------------------------------+
                                |
                                v
                      (P5) multi-thread runner
```

Every box maps to a file in `src/`. The pipeline is deterministic;
nothing here is non-reproducible, which is what makes the P13 module
cache safe to rely on across runs.

Autodiff extends this same pipeline rather than creating a separate
model format: `param` declarations enter through the frontend, the
same inlined program IR is reused, and `CompileProgramGradient`
emits a second compiled whole-program function that returns primal
outputs plus the gradient for one selected parameter.

---

## Frontend: source -> typed AST

Code: `src/lexer.{h,cpp}`, `src/parser.{h,cpp}`, `src/ast.h`,
`src/signal_program.{h,cpp}`, `src/type_check.{h,cpp}`,
`src/constant_fold.{h,cpp}`.

* `Lexer` returns tokens annotated with `SourceLoc{line, col, length}`
  (P6.1). The line is filled in by `ParseSignalProgram`, which
  tokenises line-by-line; column and length come from the lexer
  itself.
* `Parser` is a hand-rolled recursive descent. On any error it throws
  a `ParseError` carrying the bad token's `SourceLoc` plus the
  original line text, so the diagnostic renderer can print a caret-
  underlined message (`docs/dsl_real_language.md`).
* `TypeCheckSignal` (P6.3) walks the AST and enforces a tiny static
  type system (`StaticType {Number, Bool}`). It rejects implicit
  coercions (`if 1 then x else y`, `(x > 0) + 1`, ...). It runs
  BEFORE constant folding so error spans still point at the original
  operator token rather than at a folded literal.
* `FoldConstantsInPlace` (P6.2) is an AST-level constant-fold pass.
  It is structural (rewrites the tree), preserves `SourceLoc`, and
  the test suite gates its IR-equivalence (an unfolded vs folded
  program lower to byte-identical LLVM IR).

The frontend's output is a `vector<SignalDef>` where each
`SignalDef.body` is a typed, folded `Expr` tree.

With autodiff enabled, the same frontend also carries explicit
program-level parameter definitions (`param name = value`) and
parameter leaf nodes in the AST. Parameters are opt-in; ordinary
numeric literals stay constants.

---

## Whole-program inlining

Code: `src/signal_program.cpp::InlineSignalDependencies`,
`AllocateProgramNodeIds`, `BindSymbolIds`.

* `InlineSignalDependencies` topologically sorts signals, then
  replaces each `IdentifierExpr` whose name is another signal in the
  program with a deep clone of that signal's body. The result is
  that every signal's body is self-contained: no out-of-program
  identifiers, no inter-signal references. This is what lets the
  interpreter, the JIT, and the type-checker all treat one signal's
  body as a standalone Expr tree.

* `AllocateProgramNodeIds` walks every signal's body and assigns a
  globally-unique `node_id` to each stateful FunctionCall (ema, sma,
  rolling_*, zscore, vwap, lag, cross_*, rolling_corr,
  rolling_beta, kalman1d). The `node_id` indexes into the
  `SignalContext` state-vector slots; it MUST be stable across the
  program's lifetime because state lives there.

  P11 added a structural-equality dedup pass on top of this. When
  two stateful subtrees in the SAME signal body are structurally
  equal AND the first occurrence is at an "unconditional" position
  (top-level, or inside a `Conditional`'s cond, which always
  evaluates), the second occurrence reuses the first's `node_id`.
  This is what makes `if cond_using_vol && vol > 0 then raw/vol else 0`
  produce the same result in scalar and vectorized modes; without
  it, the inliner's clone produces TWO `rolling_std` state slots,
  one of which is updated only on the branch-taken side and thus
  silently desyncs. See `docs/cross_symbol_vectorization_stateful.md`
  for the full story and `test/stateful_subtree_dedup_test.cpp` for
  the regression gate.

* `BindSymbolIds` resolves the ticker identifier inside each market-
  data call (`mid(AAPL)`, `vwap(MSFT, ...)`, ...) into a numeric
  `symbol_id` baked into the `FunctionCall` node. The JIT and
  interpreter both read this directly -- there's no symbol-table
  lookup on the hot path.

---

## Backend 1: Interpreter (reference)

Code: `src/interpreter.{h,cpp}`.

The interpreter is the spec. Every JIT path -- scalar, vec, warm-
assumed, P0 lowered -- is gated against a parity test that compares
its output bit-for-bit against the interpreter. If the interpreter
gets it wrong, everything downstream is wrong.

Two cross-cutting features matter for understanding the rest of the
backend:

1. **Per-Evaluate stateful cache (P11).** `stateful_eval_cache_` is
   keyed by `FunctionCall::node_id`. Cleared at the start of each
   top-level `Evaluate` call. The first eval of a node populates
   the cache and pushes state; later evals (within the same tick
   for the same signal body) hit the cache and DO NOT push state
   again. This makes the dedup'd node_ids from
   `AllocateProgramNodeIds` produce exactly one state push per
   tick per logical operator -- matching what the JIT does at IR
   emit time.

2. **`SignalContext` slot allocation.** The interpreter calls
   `EnsureNodeCapacity` lazily on first use, so a SignalContext
   that's been through `PrewarmSignalContext` is ready for a
   stateful op's first call. The same SignalContext layout is
   shared with the JIT (this is the contract of the engine -- the
   JIT and interpreter use identical state, so swapping one for
   the other is safe at any tick).

---

## Backend 2a: Scalar JIT

Code: `src/jit_compiler.cpp::CompileProgramImpl` (the bulk of the
file).

The scalar JIT compiles all signals in a program into a single
LLVM function:

```
void signal_program_func_<id>(
    const MarketState*           market,
    MultiSymbolSignalContext*    arena,
    uint32_t                     symbol_id,
    double*                      outputs);
```

`outputs` is `num_signals` doubles (one per signal in the program,
in declaration order).

Inside the function, each signal's body is emitted in order. Mid-
expression IR is held in `CodegenContext::value_stack`; previously-
computed signals are cached in `signal_values` (a `string -> Value*`
map) so an `IdentifierExpr` that refers to another signal becomes
a SSA-Value lookup, not a re-emit.

Key sub-features that bolt onto this scalar core:

* **Stateful runtime calls (legacy path).** With
  `StatefulLoweringFlags::kNone` (or `JITSE_LOWER_STATEFUL=none`),
  operators like `ema`/`sma`/`rolling_*` emit opaque
  `call @jit_rt_<op>(...)` into `runtime.cpp`. Production default is
  **`kAll`**: inline lowered IR for every stateful op.

* **P0–P2 inline lowering (production default `kAll`).** All 14
  stateful operators have IR-lowered emitters in `jit_compiler.cpp`.
  State array bases load via `SignalContext::lowered_bases` GEP (Phase
  3) instead of `jit_rt_*_lowered_base` extern calls. On fused
  `filtered_momentum`, `runtime_call_profile` shows `jit_rt_*` sample
  share **80% → 2.8%**; fused JIT÷hand-written C++ **1.42×**
  (`bench/results/lowering_gap_phase3/`). Reach `kNone` via env/API
  for differential oracle / parity reference.

* **P1 tiered specialization.** `TieredProgramJit` compiles a
  warmup-guarded baseline immediately and lazily recompiles a
  branch-stripped "warm-assumed" specialization once the program
  has seen enough ticks. The active fn pointer is swapped via an
  atomic load.

* **P3 SIMD-prep SMA.** Hot path for `sma` with `period >= 4` and
  AVX2. The runtime helper writes the ring buffer in a contiguous
  layout the JIT can reduce with `<4 x double>` vector adds; the IR
  branches on a "full window" flag from the helper to skip the
  scalar path entirely once warmed.

* **P11 stateful-emit cache.** `CodegenContext::stateful_emit_cache`
  is a `node_id -> llvm::Value*` map. The first emit of a stateful
  FunctionCall stores its result; later emits of the SAME node_id
  return the cached Value. Combined with the dedup pass in
  `AllocateProgramNodeIds`, this guarantees exactly one runtime
  call per logical stateful op in the IR.

* **P9 observability.** After O2, the compiler captures three views:
  pre-opt IR, post-opt IR, and host-target x86-64 assembly (via a
  legacy PM run on a cloned post-O2 module). The CLI flags
  `--dump-ast --dump-ir-pre-opt --dump-ir-post-opt --dump-asm`
  surface them. `bench/llvm_mca_filtered_momentum.sh` pipes the
  asm into `llvm-mca` for port-pressure analysis.

The post-O2 module is handed to ORC's LLJIT for codegen; the
function pointer is retrieved with `lljit->lookup(fn_name)`.

---

## Backend 2c: Compiled gradients / calibration

Code: `src/autodiff.{h,cpp}`, `src/jit_compiler.cpp`
(`CompileProgramGradient`), `cli/jitse_calibrate.cpp`.

Autodiff is implemented over the same signal IR used by the
interpreter and the scalar/vector JITs.

* **Explicit parameter surface.** Programs declare trainable scalars
  with `param name = value`. Parameters live in one dense vector
  shared across the compiled program via `MultiSymbolSignalContext`.

* **Within-tick differentiation + across-tick sensitivity state.**
  Stateless algebra differentiates within the expression DAG for one
  tick. Recurrent/windowed operators (`ema_alpha`, `sma`, `lag`,
  Welford `rolling_std` / `zscore`, `rolling_corr`, `rolling_beta`,
  `kalman1d`, rolling extrema) carry parameter sensitivities in
  additional per-`(node_id, param_id)` state slots. This preserves
  the engine's warmed, allocation-free hot path.

* **Second compiled program.** `JitCompiler::CompileProgramGradient`
  emits a fused whole-program function with the shape:

  ```
  void grad_fn(
      const MarketState*,
      MultiSymbolSignalContext*,
      uint32_t symbol_id,
      int64_t param_id,
      double* outputs,
      double* gradients);
  ```

  This keeps the compiled gradient path inside the same LLVM/ORC
  pipeline as the rest of the engine.

* **Parity discipline.** `gradient_parity_test` gates interpreted
  gradients against central finite differences and compiled gradients
  against interpreted gradients at rel/abs `1e-12`.

* **Calibration demo.** `cli/jitse_calibrate.cpp` drives Adam on a
  checked-in fit-to-target CSV fixture using the compiled gradient
  function, with artifacts under `bench/results/autodiff/` and CI
  coverage via `calibration_smoke_test`.

The current Phase 4 demo is deliberately fixture-backed. The
recorded-data IC/backtest calibration path remains out of scope at
HEAD because its checked-in artifact is still broken.

---

## Backend 2b: Vectorized JIT (P2 + P10)

Code: same file, `CompileProgramVectorizedImpl`. Same pipeline shape
as the scalar JIT, but every IR value is widened to `<K x double>`.

Two distinct paths inside, depending on whether the operator is
stateless or stateful:

* **Stateless ops (P2).** `mid`, `bid`, `ask`, `spread`, arithmetic,
  conditional `select`, `abs`/`sqrt`/`log` -- all of these widen
  directly. Market data is loaded per-lane from a `<K>` array of
  `MarketState*`. LLVM keeps the IR in vector form through O2 on
  every program we've tried; `bench/cross_symbol_benchmark`
  measures the resulting throughput against the scalar baseline.

* **Stateful ops (P10 + P4).** State lives in per-symbol
  `SignalContext` slots, so state cannot be widened to a single
  `<K x double>` ring without SoA re-layout. The JIT emits per-lane
  scalarized fan-out via `EmitScalarizedFanOut`: extract lane input,
  bind lane `SignalContext`/`MarketState`, update state, insert
  result. **P4:** with default `kAll`, each lane enters
  `LaneEmitScope` and emits inline lowered IR (not opaque
  `jit_rt_*`). Parity: `vectorized_stateful_parity_test` under
  `kAll`. Perf on `filtered_momentum` (K=4): vec+lowered **~2.1×**
  vs vec+opaque, **~0.55×** vs scalar-lowered
  (`bench/results/stateful_vec_lowering_speedup.md`). True K-wide
  SIMD ring-buffer state remains future work.

  Stateless programs are program-dependent: memory-bound ~1.4× vs
  scalar JIT (`stateless_heavy.sig`); FP-heavy ~2.6×
  (`stateless_compute_heavy.sig`, `bench/results/avx2_speedup/`).

The K-lane parity test
(`test/vectorized_stateful_parity_test.cpp`) runs K independent
scalar programs in parallel and compares against one K-lane vec
program; they agree within `1e-9` (the gap is floating-point
reduction-order noise, not a correctness gap). P11's regression
test (`test/stateful_subtree_dedup_test.cpp`) further checks the
"stateful op referenced from both cond and then-branch" case across
all three execution paths.

---

## Multi-thread runner (P5)

Code: `src/multithread_eval.{h,cpp}`, benchmarks in
`bench/multithread_scaling_benchmark.cpp` and
`bench/vec_thread_composition_benchmark.cpp`.

The runner takes:

* a compiled `ProgramFn` (scalar or vec -- the runner doesn't care),
* a `vector<MarketState>` per symbol shard, and
* a `MultiSymbolSignalContext` arena large enough to hold all shards.

It spawns one thread per shard, pins each thread to a P-core when
the OS supports `sched_setaffinity`, and calls the function on each
shard's symbols in turn. The arena layout is per-symbol-contiguous,
so threads only ever touch their own slots and the cache lines
don't bounce between cores.

P12's `vec_thread_composition_benchmark` is where these two axes
get measured TOGETHER. The headline numbers
(`bench/results/vec_thread_composition/README.md`):

* Stateless program: vec(K=4)+4T is 4.34x scalar+1T.
* Stateful single-signal program: vec(K=4)+4T is 1.68x scalar+1T
  (lane axis muted, threads still scale).
* Stateful multi-signal program: vec(K=4)+4T is 3.14x scalar+1T
  (threads dominate, lanes neutral).

---

## JIT module cache (P13)

Code: same file, search for "P13:" comments;
`test/jit_module_cache_test.cpp`.

Optional. When enabled via `JitCompiler::EnableModuleCache(dir)`:

1. The compiler computes a deterministic hash over the program's
   AST canonical string (via `AstCanonicalString`), the lowering
   flags, the assume_warm flag, the lane count, the host AVX2
   feature bit, and the LLVM major version.
2. The function name is derived from the hash, so the same program
   always yields the same symbol name.
3. If the lljit ALREADY has the symbol resident (e.g. a prior
   compile in this process), the lookup returns the existing fn
   pointer immediately. No file I/O.
4. Otherwise, the compiler checks `<dir>/<hash>.<variant>.bc`. On
   hit, parse the bitcode (which IS the post-O2 module),
   skipping AST -> IR and the LLVM O2 pipeline. Only ORC codegen
   runs.
5. On miss, run the normal compile, then write the post-O2 module
   to the cache file with a temp+rename for atomicity.

The cache is per-`(program-text, flags)` and is safe to share
between processes on the same host with the same LLVM version. It
is NOT safe to share across LLVM major versions; the version is
baked into the hash key, so a stale cache from LLVM 18 will simply
miss when loaded under LLVM 19+ rather than crash.

`LastCacheHit()` reports whether the most recent compile served
from the cache; `LastCompileTimings().total_ns` is populated on
both paths. The unit test gates that a warm compile is at least 2x
faster than a cold one (a low bar that catches obvious
regressions while staying robust to host noise -- the warm/cold
ratio on the dev host was 0.41).

---

## Runtime: rolling statistics (Welford)

Code: `src/runtime.cpp` (`RingStatsState`, `RingStatsPush`,
`RingStatsStddevSample`).

`rolling_std` and `zscore` read sample standard deviation from an
O(1) Welford accumulator (`mean`, `m2`) updated on every push.
The two-pass long-double loop is preserved as
`RingStatsStddevSampleTwoPassReference` for the parity oracle only.

Pure rolling Welford drifts on catastrophic-cancellation inputs
(large mean, tiny variance). The production path refreshes `mean`
and `m2` from the ring buffer once every `capacity` slide operations
(amortized O(1)), bounding drift to one window of long-double
roundoff. Gated by `test/welford_stddev_parity_test.cpp`.

---

## Live ingest: SPSC ring + pipeline bench

Code: `src/spsc_ring.h`, `bench/spsc_jit_pipeline_bench.cpp`.

A lock-free single-producer / single-consumer ring (cache-line
padded head/tail, acquire/release ordering, cached opposing index)
connects a feed thread to an eval thread that pops events, updates
`MarketState`, and calls the fused JIT program. Latency is
`out_ns - enqueue_ns` using `steady_clock` (VDSO `CLOCK_MONOTONIC`
on Linux).

This is not the default `main.cpp` replay loop; it exists to
measure end-to-end ingest→signal latency and to gate the ring with
`spsc_ring_test` (FIFO, 1M-message stress, zero hot-path
allocations). Artifacts: `bench/results/spsc_pipeline/`. On WSL2,
tail percentiles reflect host noise; p50 matches the expected
~210 ns overhead on top of standalone JIT latency (~19 ns).

---

## Fuzzing (P8)

Code: `fuzz/`, gated by `JITSE_BUILD_FUZZERS=ON` for libFuzzer
mode (Clang + sanitizers), corpus-driver smoke tests otherwise.

Two harnesses:

* `parser_fuzzer` feeds raw byte streams through `ParseSignalProgram`.
  Catches parser crashes, unbounded recursion, UB from malformed
  tokens. Seed corpus is in `fuzz/corpus/parser/`.
* `runtime_fuzzer` generates random ASTs from a curated grammar,
  feeds them random tick streams, and asserts interpreter vs JIT
  bit-equality (NaN-aware `memcpy` compare). Seed corpus in
  `fuzz/corpus/runtime/`. This is what catches "JIT optimised
  away a runtime call it shouldn't have" or "interpreter rounded
  differently from the JIT" bugs that the curated parity tests
  miss.

CI runs both in standalone-driver mode (no sanitizers) so any
compiler can build them. The libFuzzer mode runs locally during
campaigns and pushes new findings into the corpus, which CI then
replays.

---

## File map (where to look)

| What | Where |
| --- | --- |
| DSL tokens / parser / AST | `src/lexer.*`, `src/parser.*`, `src/ast.h` |
| Type system, constant folding | `src/type_check.*`, `src/constant_fold.*` |
| Whole-program assembly | `src/signal_program.*` (parse, inline, allocate, bind) |
| Reference interpreter | `src/interpreter.*` |
| Runtime helpers + state | `src/runtime.*` |
| LLVM JIT (scalar + vec + cache) | `src/jit_compiler.*` |
| Tiered (warm vs baseline) | `src/jit_compiler.*` (`TieredProgramJit`) |
| Multi-thread runner | `src/multithread_eval.*` |
| SPSC ring | `src/spsc_ring.h` |
| DSL formatter | `src/dsl_formatter.*`, `cli/jitse_fmt.cpp`, `cli/jitse_lint.cpp` |
| Main CLI | `src/main.cpp` |
| Benchmarks | `bench/*.cpp` |
| Roadmap artifacts | `bench/results/`, `docs/` |
| Per-feature design docs | `docs/dsl_real_language.md`, `docs/new_operators.md`, `docs/fuzzing.md`, `docs/compiler_observability.md`, `docs/cross_symbol_vectorization_stateful.md` |
| Tests | `test/*.cpp` (also serve as executable examples) |
| Fuzzers | `fuzz/*.cpp`, `fuzz/corpus/` |
| Roadmap, P-tasks | `PROJECT_ROADMAP.md` |

---

## Invariants worth knowing

* **One state slot per logical stateful operator.** Across
  `AllocateProgramNodeIds`'s dedup, the JIT's `stateful_emit_cache`,
  and the interpreter's `stateful_eval_cache_`, every logical op
  is pushed exactly once per tick per signal evaluation.

* **`SignalContext` is shared by all three backends.** The interpreter
  and both JIT paths (scalar + vec) read/write the same in-memory
  layout. Swapping one for another at any tick boundary is a
  supported operation and is exercised by the tiered JIT.

* **The cache is observation-transparent.** Enabling the module
  cache changes timings, not outputs. The unit test gates this
  explicitly by running a cached vs uncached compile and bit-
  comparing the per-tick output.

* **The vec JIT is a strict superset of the scalar JIT's
  language.** Any program the scalar JIT accepts also compiles in
  vec mode (with the one exception of P0-lowered stateful ops, by
  design).

* **Source locations survive every pass.** `Expr::loc` is preserved
  by `CloneExpr`, `FoldConstantsInPlace`, and `InlineExpr`, so a
  diagnostic raised inside the JIT can still point back at the
  user's original line and column.
