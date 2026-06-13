# P8 — Real fuzz infrastructure

The repo previously had "fuzz parity" tests that generated random
expression trees and asserted interpreter == JIT bit-equality on the
generated AST. That is useful for AST-space coverage but it is not a
structural fuzzer: it never feeds raw bytes to the parser, and it
never explores the input space with coverage feedback.

P8 adds two libFuzzer-based harnesses:

| Harness                          | What it stresses                                                   |
|----------------------------------|---------------------------------------------------------------------|
| `fuzz/parser_fuzzer.cpp`         | `ParseSignalProgram(bytes)` — type checker + constant folder + lexer + parser surface |
| `fuzz/runtime_fuzzer.cpp`        | Interpreter vs JIT bit-equality on a randomized AST + random tick stream |

Both harnesses are dual-mode:

1. **libFuzzer mode** (`JITSE_BUILD_FUZZERS=ON`, Clang only). Compiles
   with `-fsanitize=fuzzer,address,undefined`. The libFuzzer driver
   mutates the seed corpus to maximize edge coverage and runs each
   input through `LLVMFuzzerTestOneInput`.
2. **Standalone corpus-driver mode** (default; any compiler). The
   harness compiles with `JITSE_FUZZ_LINK_DRIVER=1`, which enables a
   `main()` that walks `fuzz/corpus/` and runs each file through
   `LLVMFuzzerTestOneInput`. This becomes a CTest smoke gate
   (`parser_fuzzer_smoke`, `runtime_fuzzer_smoke`) that any CI builds
   regardless of compiler.

## Running a real libFuzzer campaign

One-time setup:

```bash
mkdir -p build-fuzz && cd build-fuzz
CC=clang CXX=clang++ cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                              -DJITSE_BUILD_FUZZERS=ON
cmake --build . --target parser_fuzzer runtime_fuzzer -- -j
```

Then drive each harness for a configurable wall-clock budget:

```bash
bench/run_fuzzers.sh 3600        # one hour per harness (default)
bench/run_fuzzers.sh 60          # one minute smoke
bench/run_fuzzers.sh 3600 build-fuzz   # explicit build dir
```

Each campaign writes a discovery log to
`bench/results/fuzz/<harness>_<UTC>.log`. Any input that triggers a
crash, UB, or an interpreter/JIT divergence is left in the corpus
directory so it can be replayed deterministically by the standalone
smoke harnesses in CI.

## What each harness checks

### parser_fuzzer

`ParseSignalProgram(bytes)` runs the full source-language pipeline:

1. Per-line lexing with source location stamping.
2. Recursive-descent parsing.
3. Static type checking (P6.3).
4. AST-level constant folding (P6.2).
5. Dependency inlining + node ID + symbol binding (in
   `InlineSignalDependencies`).

The harness asserts:

* No undefined behavior under `-fsanitize=undefined`.
* No memory-safety bug under `-fsanitize=address`.
* No uncaught throw of a non-`std::exception` type (such a throw would
  escape `parser_fuzzer` and crash via `std::terminate`, which
  libFuzzer reports).

`std::exception` throws are normal parse rejections and are caught
silently. The input is capped at 64 KB to prevent adversarially-long
inputs from melting the lexer's `O(n)` token loop into a CPU-bound
non-crash.

### runtime_fuzzer

The harness derives a 64-bit seed from the fuzz input and uses it to
generate a randomized but well-shaped AST (depth 2-4, mix of
arithmetic, comparisons, conditionals, `mid`/`spread`/`abs`/`sqrt`).
It pumps the same random tick stream through:

* The **interpreter** (reference).
* The **JIT** (compiled with the default lowering flags).

For each tick, the harness asserts bit-equal output:

```c++
NaN == NaN          -> equal
finite a == b       -> std::memcpy bit-identical
finite vs NaN/Inf   -> not equal -> std::abort()
```

This is tighter than the parity tests' `1e-9` tolerance because both
paths share the same C++ floating-point helpers; any divergence is a
real JIT codegen or dispatch bug.

The generated programs are intentionally stateless (no `sma`/`ema`/
`lag`) to avoid the need to plumb state allocation through the fuzz
harness. The parity tests cover the stateful AST shapes; the runtime
fuzzer is the wide-coverage gate for stateless AST shapes.

## Seed corpus

`fuzz/corpus/` ships with hand-written seeds:

| File                       | Intent                                                    |
|----------------------------|------------------------------------------------------------|
| `01_simple.sig`            | Trivial happy path: single signal, one operator.           |
| `02_arithmetic.sig`        | Cross-signal arithmetic + literal coefficients.            |
| `03_conditional.sig`       | Conditional-with-comparison: covers IR branch lowering.    |
| `04_stateful.sig`          | All P0-lowered ops + rolling_std + conditional combination.|
| `05_pathological.sig`      | Deep nested arithmetic for AST depth probing.              |
| `10_unterminated.sig`      | Unclosed `(` — parser must reject without crashing.        |
| `11_random_chars.sig`      | Lexer-level garbage — lexer must report cleanly.           |
| `12_empty.sig`             | Empty input.                                               |
| `13_long_number.sig`       | Malformed numeric literal sequence.                        |

libFuzzer mutates from this seed set; after a real campaign the corpus
typically grows to 50–100 entries. The new entries are inputs that
libFuzzer's coverage tracker identified as exploring previously
unvisited edges in the parser/runtime.

## Reading the libFuzzer output

A typical log line looks like:

```
#4752 NEW    cov: 25 ft: 66 corp: 25/2232b lim: 168 exec/s: 0 rss: 110Mb L: 145/168
```

* `cov: 25` — number of basic blocks the corpus reaches.
* `ft: 66` — number of features (coverage edge + value-profile)
  observed.
* `corp: 25/2232b` — 25 corpus entries totaling 2232 bytes.
* `L: 145/168` — current input is 145 bytes; longest corpus entry is
  168 bytes.

`NEW` means this iteration discovered a new feature; `REDUCE` means
libFuzzer minimized an existing input.

A finding shows up as:

```
==12345==ERROR: AddressSanitizer: ...
SUMMARY: AddressSanitizer: heap-buffer-overflow ...
artifact_prefix='./'; Test unit written to ./crash-<sha1>
```

The `crash-<sha1>` file goes into `fuzz/corpus/runtime/` (or
`fuzz/corpus/` for parser crashes), and CI replays it on every
subsequent build via the standalone smoke harness.

## Validation runs so far

* `parser_fuzzer -runs=5000` (Clang 21, ASan+UBSan, baseline corpus):
  zero crashes, corpus expanded from 9 seeds to 67 entries. The 58
  added entries are coverage-driven inputs that probe unfamiliar
  lexer/parser edges (long numbers, comment edges, EOF inside
  tokens, etc.). All 58 are replayed by the smoke harness on every
  build.
* `runtime_fuzzer -runs=500 -max_total_time=60`: zero interpreter/JIT
  divergences across 500 random-shape ASTs and ~16,000 ticks.

These are not "we ran for an hour" numbers and we don't claim they
are. They are evidence that the harness is correctly wired and finds
no immediate bugs. For a real campaign run `bench/run_fuzzers.sh
3600` on a beefier host and check in the resulting `bench/results/
fuzz/*.log`.

## Build matrix

| Mode                                | Compiler | Sanitizers              | Where used                |
|-------------------------------------|----------|-------------------------|---------------------------|
| `JITSE_BUILD_FUZZERS=OFF` (default) | any      | none                    | CI smoke (ctest)          |
| `JITSE_BUILD_FUZZERS=ON`            | Clang    | fuzzer + address + UB   | Local libFuzzer campaign  |

CTest targets `parser_fuzzer_smoke` and `runtime_fuzzer_smoke` exist
only in the default mode. The libFuzzer-real binaries (`parser_fuzzer`
/ `runtime_fuzzer`) do not register a CTest entry because their
runtime is unbounded by design.
