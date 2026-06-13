# jit-signal-engine — Project Walkthrough

This is the document you read to understand the project. Not a reference, not a checklist — a guided tour. By the time you're done, you should be able to sit across from an engineer, sketch the architecture on a whiteboard, defend every design choice, and admit honestly what you didn't solve.

Read it in order. The early sections build the picture; the later ones go deep on the parts that take real work.

---

## What this is, in plain words

Imagine you work at a trading firm. The quants want to write things like:

> "Buy when the short moving average crosses above the long one, but only if volatility is positive."

If you make them write that in C++, they hate you. Every change requires a recompile and a code review. If you let them write it in Python, the engine becomes the bottleneck at a million ticks per second.

The compromise that real systems use is: **give the quants a small domain-specific language, then compile it at startup into native code that runs at C++ speed**. That's what this project is. It's a small language for trading signals, a runtime that executes the language, and — the interesting part — a real LLVM JIT that turns the signals into machine code.

There are two execution paths in the codebase. One is an **interpreter**, which is simple and predictable. The other is the **JIT**, which is fast. The interpreter is treated as the source of truth — whenever you change something in the JIT, the rule is "it must still match the interpreter to the bit." That gives a clean correctness story without making the JIT itself simple.

You could describe the project in one sentence: it's a C++20 DSL for trading signals with an LLVM ORC v2 JIT backend, validated against an interpreter oracle, and benchmarked under conditions you can actually defend.

---

## The shape of the system

The path from "signal written on disk" to "number coming out per tick" looks like this:

```
   .sig source file
           │
           ▼  (lexer + parser)
       signal AST
           │
           ▼  (program builder — dependency inlining)
   program graph with stable node IDs
           │
           ├──────────────────┬─────────────────┐
           ▼                  ▼                 ▼
      interpreter           JIT             SIMD JIT
       (oracle)         (production)       (when AVX2)
           │                  │                 │
           └──────────────────┴─────────────────┘
                              ▼
                  runtime + market state
                              ▼
                  one number per signal per tick
```

The thing worth noticing is that the front of the pipeline — lexing, parsing, building the program graph — runs **once**, at startup. Everything to the right of "program graph" is the hot path. That's where every nanosecond matters.

When a tick arrives, the engine doesn't re-parse anything. It calls a function pointer (in the JIT case) or runs a tiny AST walker (in the interpreter case), and gets back signal values. Everything stateful — like the running mean inside an EMA — lives in pre-allocated C++ memory that's indexed by a stable integer ID assigned during compilation. There are no maps, no hash lookups, no allocations on the hot path. That's the design invariant the test suite enforces.

---

## The language itself

The DSL is small on purpose. It does five things:

It does **arithmetic and comparison** — the boring stuff you'd expect.

It has **conditionals** — `if condition then a else b`, which compiles to a `select` instruction in IR rather than a branch.

It can **read market data** — `bid(SYMBOL)`, `ask(SYMBOL)`, `mid(SYMBOL)`, `last(SYMBOL)`. Each of these is, under the hood, a load from a fixed-size array indexed by the symbol's integer ID. There's no hashing of ticker strings at runtime — that happens once, at parse time.

It has **math built-ins** — `abs`, `log`, `sqrt`. These get lowered directly to LLVM intrinsics in the JIT, and they have their own parity test against the interpreter so a regression here gets caught immediately.

And it has **stateful operators** — `sma`, `ema`, `rolling_std`, `rolling_min`, `rolling_max`, `lag`, `cross_over`, `cross_under`. These are the operators that need to remember things from previous ticks. They're the heart of any real signal system, and they're where most of the interesting design decisions live.

A real signal program ends up looking like this:

```
short_ma = sma(mid(AAPL), 10)
long_ma  = sma(mid(AAPL), 50)
vol      = rolling_std(mid(AAPL), 30)
raw      = (mid(AAPL) - lag(mid(AAPL), 5)) / lag(mid(AAPL), 5)
filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0
```

This is the canonical example — `filtered_momentum.sig` — and it shows up in every benchmark. Five signals, lots of shared inputs. Notice how many times `mid(AAPL)` appears. That repetition is the seed of an interesting optimization story we'll get to.

---

## How a `.sig` file becomes machine code

Walk through it with me.

**Step one — the lexer.** Hand-written in C++. Reads characters, emits tokens. Nothing exotic; it just keeps the rest of the codebase from doing character arithmetic.

**Step two — the parser.** Recursive descent. Each signal becomes an AST: a tree of operator nodes and leaves. The parser handles precedence, parens, comparisons, the `if/then/else` syntax. At this point, every signal is a self-contained tree.

**Step three — the program builder.** This is where things get more interesting. A real program isn't one signal — it's five, like the example above, with `filtered` referencing `short_ma` and `long_ma` and `vol` and `raw`. If we naively compiled `filtered` by inlining the bodies of its dependencies, we'd end up with **two separate `sma` state slots** — one for the standalone `short_ma` and one inside `filtered`'s inlined copy. They'd both advance per tick, both have their own mean, and the answer would be subtly wrong.

The fix — which lives in `signal_program.cpp` — is to do dependency resolution **across the whole program** before allocating any state. Each operator-with-state gets one stable integer ID, and any signal that depends on `short_ma` reads from that one slot. This was a real bug; the audit log records the fix.

The output of this stage is a topologically-ordered list of nodes, each with a stable `node_id` that determines where its state lives in memory.

**Step four — the codegen.** Now we hand the program graph to either the interpreter or the JIT.

The interpreter walks the AST per tick. For an `add` node, it evaluates both children and adds. For an `ema` call, it dispatches to a C++ helper that owns the state for that node ID. Simple and slow.

The JIT walks the same program graph, but instead of evaluating, it **emits LLVM IR**. For arithmetic and control flow, the IR is inlined directly. For market loads, it's a `load` from a global pointer plus an index. For stateful operators, it emits a `call` to a C++ runtime function — `jit_rt_ema`, `jit_rt_sma`, etc. — that owns the state for that node. Once the whole program's IR is built, LLVM's O2 pipeline runs over it, and ORC v2 hands back a function pointer.

That function pointer is your hot path. Every tick, you call it.

---

## The choice that shapes everything: state in C++, not in IR

The most consequential design decision in this codebase is that **stateful operators stay in C++ instead of being lowered into IR**. It's worth understanding why, because every interviewer will ask.

Consider what it would take to lower `rolling_std` into pure IR. You'd need a ring buffer in static memory or stack allocation; you'd need indexing arithmetic to handle the modular wraparound; you'd need a numerically stable variance formula (Welford or two-pass), which means non-trivial control flow inside the inner loop. You'd duplicate that logic across every variance-like operator. And every time you found a numerical bug, you'd fix it twice — once in the interpreter's helper and once in the IR-lowered version.

By keeping all the stateful logic in `runtime.cpp`, both the interpreter and the JIT call the same function. There's one place for the EMA update, one place for the ring buffer indexing, one place for the variance calculation. When a numerical bug shows up — and one did — you fix it once and both paths benefit.

The cost of this choice is that every stateful op is a function call from the JIT'd code. LLVM cannot inline `jit_rt_ema` because it doesn't have the source. That's a real performance cost, and it's part of why the headline speedup is "5×" and not "50×". The arithmetic and dispatch overhead is what the JIT eliminates; the stateful ops still pay the call cost. That's an honest trade.

---

## The numerical-stability bug that taught a lesson

`rolling_std` originally used the standard formula every textbook gives you:

```
variance = E[x²] − (E[x])²
```

This works fine in theory. In floating-point arithmetic on similar-magnitude price series, it's a disaster. The two terms can be nearly equal — say, both around 10⁸ — and their subtraction cancels the high bits, leaving you with garbage in the low bits. On a flat input window, the result can be a tiny negative number, which when you take the square root produces NaN.

The fix in the audit log replaces it with a **two-pass long-double calculation over the ring buffer**. First pass: compute the mean. Second pass: sum the squared deviations from that mean. Slower per call, but it's the kind of slower that's a rounding error in the overall hot path, and it gives correct answers on flat windows.

The reason this story matters in an interview is that it shows two things at once: you know enough about IEEE 754 to spot a cancellation bug, and you have the test infrastructure (the differential oracle, which we'll get to) to catch it. Both are signals interviewers actively look for.

---

## The big optimization: deduplicating market loads

Back to the canonical example. `filtered_momentum.sig` calls `mid(AAPL)` directly or indirectly **eleven times** across its five signals. Naively, the fused JIT would emit eleven `load` instructions per tick. That's eleven memory operations for the same value.

You'd think LLVM's O2 pass would notice and CSE them. It can't. The reason is subtle: between two consecutive `mid(AAPL)` loads, there are calls to `jit_rt_*` functions — `jit_rt_sma`, `jit_rt_ema`, and so on. From LLVM's perspective, these are opaque function calls with potentially arbitrary side effects, possibly including writes to the market data memory. LLVM has no way to prove that they don't, so it conservatively refuses to CSE across them.

We know better. Market loads are read-only and pure within a tick. So the fix lives in `EmitMarketFieldLoad` and `PrewarmProgramMarketLoads` in `src/jit_compiler.cpp`. The codegen does two things. First, **at program entry**, it preloads each `(symbol_id, field)` pair into a fresh SSA value — one `bid(AAPL)` load, one `ask(AAPL)` load, regardless of how many times those appear in the program. Second, when the codegen later encounters a `bid(AAPL)` call in the AST, it doesn't emit a new `load` — it returns the SSA value from a memo table keyed by `(symbol_id, field)`.

The before/after IR diff lives in `bench/results/cse_evidence/cse_diff_verified.md`, and the numbers are stark: **22 market loads collapse to 2**, before O2 even runs. The `cse_load_dedup_test` codifies this as a regression check.

Why does this matter for performance? Memory loads aren't free even from L1 cache. More importantly, every load has alias-analysis implications for the surrounding IR. Removing 20 redundant loads gives the optimizer cleaner ground to work on for everything else.

---

## Going multi-symbol, then going parallel

A single-symbol engine is a toy. Production systems handle thousands of symbols. The design moves through two stages.

**Stage one: multi-symbol structure of arrays.** Instead of a separate engine per symbol, there's one `MultiSymbolSignalContext` that owns a per-symbol state arena. The compiled JIT function takes `symbol_id` as a parameter. When `jit_rt_sma` is called inside the JIT for node 7, it indexes `context.sma_states[symbol_id][node_id=7]` and updates that slot. One compile, many symbols, all state correctly isolated by construction.

**Stage two: sharded multi-threading.** Symbols are partitioned across N threads. Each thread gets its **own** `MultiSymbolSignalContext` — meaning its own private state arena for its slice of the symbols — but they all call into the same compiled JIT function (the function is just code, it's safe to share). After each batch, results are hashed in symbol order so the merged output is bit-identical to what a single thread would have produced.

The bit-identical part is enforced by `multithread_equivalence_test`. This is not a "they should be very close" test; it's a hash check. Any non-determinism is a failing test.

Scaling was measured three ways on the canonical host (`wsl2-ultra9-275hx-2026-05`), each a sustained 60s run on 10,000 symbols:

| Regime | Peak | Scaling | Efficiency |
|--------|------|---------|------------|
| **P-cores only** (cores 1–7) | 6 threads | **5.45×** | **90.8%** |
| **E-cores only** (cores 8–23) | 16 threads | **6.51×** | **40.7%** |
| **Hybrid** (mixed P+E, default) | 16 threads | **5.69×** | **35.6%** |

**Lead with the P-core row in interviews.** On homogeneous Lion Cove P-cores, the sharded JIT path scales to **90.8% parallel efficiency** at 6 threads — strong evidence that the algorithm and shard design are sound. The hybrid 35.6% is not a software bug; it's what happens when you pin 16 threads across P-cores and E-cores at different frequencies on an Intel Ultra 9 275HX.

The E-core run is supplemental: higher peak throughput at 16 threads (6.51×) but lower efficiency (40.7%), and the 4-thread point is actually slower than 2-thread on E-cores — that's reported as measured, not smoothed. Core mapping is documented in `bench/PINNED_HOST.md` (WSL2 doesn't expose per-CPU MAXMHZ; mapping uses `bench/verify_core_clusters.sh` plus standard 275HX enumeration: CPUs 0–7 P, 8–23 E; CPU 0 reserved).

Reproduce P-only: `bash bench/run_pinned_multithread_scaling.sh build-wsl --cores 1,2,3,4,5,6,7 --thread-counts 1,2,4,6`. Artifacts: `bench/results/multithread_scaling_pcores.{json,md}`.

---

## How we know the JIT is right

Correctness in a JIT is genuinely hard. You can write tests for individual operators, but the JIT also does fusion, optimization, and lowering. The way the codebase deals with this is to layer evidence from weak to strong.

The weakest layer is **unit tests on the operators themselves** — does `sma` produce the right mean? does the parser parse `if/then/else` correctly?

A layer up is **fuzz parity**. You generate random expressions of bounded depth and you check that the interpreter and the JIT produce the same answer to floating-point tolerance. This catches the kind of bug that no single hand-written test would: weird precedence interactions, unusual nesting, etc. There are three of these tests — the basic one, a SIMD variant that compares scalar JIT to AVX2 JIT, and a multi-symbol variant that compares single-symbol reference to multi-symbol execution.

A layer up from that is **a deterministic CTest fixture**. A small synthetic AAPL input with handcrafted ticks runs through the engine, and the output is compared against a known-good reference within tight tolerances (relative 1e-6, absolute 1e-9). This fixture currently hits 100% within tolerance on all five signals in the canonical program. It's gated at 99.9% — if anything regresses, the test fails.

And at the top is the **pandas differential oracle**. This is the one to lead with in an interview. It does the following: run the C++ JIT engine over a 1-million-event slice of real recorded ITCH market data; for the same input, compute the same five signals in **pandas**, in Python, with completely independent code; then diff the two outputs row-by-row.

The reason this is a strong test is that pandas isn't an extension of the C++ engine — it's a totally separate implementation in a different language, with its own floating-point semantics. If both agree to 1e-6 across a million ticks, the C++ engine is almost certainly correct. The current artifact shows `short_ma`, `long_ma`, and `raw` matching at 100% within tolerance, `vol` at 99.74%, and `filtered` at 99.72%. The residual sub-percent is documented as warmup and degenerate-window edge cases, not actual numerical disagreement.

Building this oracle had its own bug story. The first version computed `rolling_std` globally instead of per output symbol, which was the wrong semantics; fixing the per-symbol replay matched the JIT exactly. That's the kind of detail that, if you mention it in an interview, signals you actually built the thing.

---

## The numbers, and what they mean

The headline numbers in this project are not "10×" or "13×". They used to be, on a different host with different conditions, and those numbers still exist in the historical CSVs in the repo — but you do not lead with them, because they are not reproducible on the canonical host.

What you lead with is this: on a documented WSL2 host pinned to one core with a fixed seed and 30 batches of one million events each, the JIT produces a **5.18× median speedup over the interpreter on the momentum signal** with a 95% bootstrap confidence interval of [4.92×, 5.36×]. On the fused five-signal program, the speedup is **2.96× median** with a [2.50×, 3.53×] confidence interval. The fused number is lower because the rolling state ops dominate the cost there, and we've already discussed why those don't accelerate as much as pure arithmetic.

Two things to notice about those numbers. First, they have confidence intervals from a real bootstrap — not just point estimates someone eyeballed once. Second, they cite a specific host (`wsl2-ultra9-275hx-2026-05`), a specific core, a specific seed, a specific build. If someone asks "can I reproduce this?", the answer is "yes, run `bash bench/run_pinned_speedup.sh build-wsl` on the same host." The benchmark provenance lives in `bench/PINNED_HOST.md`.

The multi-thread headline for resumes: **5.45× at 6 P-cores, 90.8% efficiency** (`multithread_scaling_pcores.md`), with bit-identical output verified by hash comparison. Keep hybrid (5.69× / 35.6%) as context for "why mixed-core pinning looks bad," not as the lead number.

Latency numbers exist too — p99 in the 10–80 nanosecond range depending on the signal — but those are reported as ranges with the artifact, not as a single number. The canonical advice in `EVIDENCE.md` is: every performance claim cites an artifact, every artifact cites a host.

---

## What this project doesn't do

Lying about scope is the easiest way to lose credibility in an interview. Here's the honest list.

There's **no live exchange connection**. This consumes the canonical journal produced by the sister project, `market-data-handler`, which itself runs offline. You can say "I built a JIT engine that integrates with our market data pipeline" — that's true. You cannot say "I built a live trading engine."

There's **no GPU backend**. The codebase is CPU-only. A GPU JIT was considered and explicitly rejected from the upgrade plan because it overlapped too much with the `uTPU` project's scope.

There's **no profile-guided optimization**. The JIT runs LLVM's O2 pipeline and stops. PGO is an interesting roadmap item — replay a trace, collect branch profiles, feed them back — but it's not implemented.

The **SIMD path is not always a speedup**. It's correct and parity-tested, but on the specific benchmark grid of `sma(64)`, `sma(128)`, and `zscore(128)`, AVX2 did not beat scalar. The honesty note for this lives in `docs/simd_candidates.md`. The path exists because it's correct infrastructure; the claim is "we lower to SIMD when AVX2 is available and verify parity," not "we always win with SIMD."

Do **not** lead with hybrid **35.6%** efficiency and pretend the engine doesn't scale — on P-cores alone it's **90.8%** at 6 threads. Hybrid mixed pinning is the honest explanation for the lower number, not an algorithmic ceiling.

---

## Sitting across from an engineer

Here are the questions you should expect, and how to answer them naturally.

**"Why LLVM ORC v2 instead of MCJIT?"**

Because ORC v2 is the actively maintained LLVM JIT API, with a cleaner symbol resolution model via `JITDylib`, first-class thread safety, and a better story for long-running services. MCJIT is legacy — it works, but the LLVM project itself has moved on. For a from-scratch project, picking the supported API was an obvious call.

**"How do you know the JIT is correct?"**

Layered evidence. Per-operator unit tests at the bottom. Fuzz-parity tests above that — randomly generated expressions where the interpreter and JIT must produce bit-identical output, in three flavors (basic, SIMD vs scalar, multi-symbol). A deterministic CTest fixture that gates at 99.9% within tolerance and currently hits 100%. And at the top, a pandas differential oracle that runs the same signals in Python on a million events of real recorded ITCH data — pandas being an entirely independent implementation in a different language, which is what makes the test strong.

**"Why are stateful operators function calls instead of inlined IR?"**

Historically they were opaque `jit_rt_*` calls for correctness isolation — we caught a real `rolling_std` cancellation bug in C++ exactly that way. Production default is now **`kAll`**: all 14 stateful ops emit inline lowered IR, and on fused `filtered_momentum` the profile shows `jit_rt_*` dropping from **~80% to ~3%** of samples. We keep `kNone` reachable for differential oracle / parity reference. The fused JIT÷hand-written C++ gap closed to **1.42×** after lowering + ctx GEP bases (`lowering_gap_phase3/`).

**"Walk me through the market-load deduplication."**

The naive emission of the fused program produced 22 `load` instructions per tick because the same `mid(AAPL)` was called eleven times across the five signals. LLVM's O2 couldn't CSE them because each pair of loads is separated by an opaque `jit_rt_*` call, and LLVM has to assume those calls could write to market data memory. We know they can't. So the codegen does two things: it preloads each `(symbol_id, field)` pair once at program entry, and it memoizes the resulting SSA value in an emitter-side map so subsequent references in the AST get the same value back. 22 loads collapse to 2, verified by an IR diff artifact. Test: `cse_load_dedup_test`.

**"Why isn't your multi-thread scaling closer to linear?"**

It is, on homogeneous P-cores: **5.45× at 6 threads, 90.8% efficiency** on cores 1–7, with `multithread_equivalence_test` proving bit-identical output. The **35.6%** number is the hybrid default — 16 threads pinned across P-cores and E-cores on an Ultra 9 275HX. E-cores run slower; mixing them drags efficiency even though peak hybrid throughput (5.69×) is in the same ballpark. We re-ran P-only and E-only regimes (`multithread_scaling_pcores.md`, `multithread_scaling_ecores.md`) to separate hardware from algorithm. There's no shard contention — each thread owns disjoint symbols and a private state arena.

**"What's the hardest bug you fixed?"**

The `rolling_std` cancellation. Original code used `E[x²] − E[x]²`, which is numerically unstable when both expectations are similar magnitudes. On a flat input window, the subtraction cancelled the high bits and left us with garbage in the low bits, sometimes a small negative number, which when square-rooted gave NaN. The fix was a two-pass long-double calculation over the ring buffer — compute the mean first, then sum squared deviations from it. Caught by the differential oracle, fixed in `runtime.cpp`, regression-tested by the oracle and the CTest fixture.

**"What would you do next if you had another month?"**

Three things. First, **K-wide SIMD ring-buffer state** for lowered stateful ops in vector mode — P4 landed per-lane lowered fan-out (parity green), but vec+lowered is still **~0.55×** scalar-lowered on `filtered_momentum`; true `<K x double>` SoA state is the likely path to a perf win. Second, profile-guided optimization — replay a representative tick trace, collect branch profiles, feed them back through LLVM (`assume_warm` tiering exists; broader PGO is open). Third, optional: bare-metal SPSC/latency rerun for tighter p99 claims beyond WSL scheduler noise, or cross-signal structural stateful CSE (blocked on fused `Evaluate` parity today).

---

## Phrases that will hurt you

If you say "10× to 13× speedup" you'll get asked which artifact, and you'll have to admit that's the historical number on a different host, and now you've lost the room. Say "5.18× median on the pinned-host momentum benchmark with bootstrap CI 4.92× to 5.36×" instead.

If you say "linear multi-core scaling" without qualification you'll get pressed on hybrid runs. Say "**90.8% parallel efficiency on P-cores at 6 threads (5.45×)**; hybrid mixed P+E at 16 threads is 35.6% because of E-core frequency mismatch, not shard contention."

If you say "beats pandas" you've completely missed the point. Pandas is the **oracle**, not the competition. The whole point is that an interpreted Python library produces the same answers we do, which is what gives us confidence in our compiled C++ JIT.

If you say "live trading" you'll be asked which exchange, what's the wire protocol, what's the order management story, and you'll have to admit it's all offline. Say "offline DSL execution engine that integrates with a recorded market data pipeline."

---

## Things to keep in your head

The integer node ID is the linchpin. Stable across the whole program, allocated once at compile time. That's why dependency inlining is safe (no duplicated state) and why per-tick eval is cheap (array indexing).

The market-load memo key is `(symbol_id, field)`. Not the call site. That's what makes 22 → 2 work.

State isolation happens at two levels for the multi-thread path: each thread owns a separate `MultiSymbolSignalContext`, and within each context, each symbol has its own state slot indexed by `symbol_id`. Both layers matter.

The differential oracle is **pandas** in Python, not numpy. Different floating-point semantics is half of why it's a strong test.

The `rolling_std` bug is the story to tell. It's concrete, it's about numerical analysis, and it has a clean fix.

Historical 10×–13× numbers exist. They're labelled Historical in the claims matrix. Never lead with them.

Multi-thread resume headline: **5.45× / 90.8%** on P-cores @ 6T. Hybrid **5.69× / 35.6%** is mixed-core context, not the scaling story.

---

## Where to read more

If you want the rapid-fire Q&A version after this, `docs/interview_prep.md` is the deep companion — shorter answers, more questions, designed for cramming the morning of.

If you want to know what claim is allowed and what isn't, `CLAIMS_MATRIX.md` is authoritative. Every resume bullet has a row there with its current status.

If you want to reproduce numbers, `EVIDENCE.md` is the claim-to-artifact-to-command map.

If you want to know what's done and what's not, `PROJECT_ROADMAP.md` and `NEXT_TASK.md` are the current truth.

If something here disagrees with `CLAIMS_MATRIX.md`, trust the matrix.
