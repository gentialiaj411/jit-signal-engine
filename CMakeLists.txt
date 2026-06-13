cmake_minimum_required(VERSION 3.20)
project(jit_signal_engine LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
  message(STATUS "No build type specified, defaulting to Release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-fno-omit-frame-pointer)
  endif()
endif()

option(JITSE_ENABLE_LLVM "Enable LLVM ORC JIT backend" ON)

# P8: libFuzzer harnesses for parser + runtime parity.
#   OFF (default): builds parser_fuzzer and runtime_fuzzer as standalone
#     corpus-driver binaries that exercise the same TestOneInput entry
#     points without linking libFuzzer. This works with any compiler and
#     gives CI a smoke-test path.
#   ON: requires Clang. Builds the harnesses with
#     `-fsanitize=fuzzer,address,undefined` so they can be driven by
#     libFuzzer for real fuzz campaigns. The standalone smoke tests are
#     skipped (the binaries no longer have a main()).
option(JITSE_BUILD_FUZZERS "Build libFuzzer-driven fuzz harnesses (requires Clang)" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_library(signal_core
    src/autodiff.cpp
    src/ast_utils.cpp
    src/ast_clone.cpp
    src/ast_printer.cpp
    src/constant_fold.cpp
    src/dsl_formatter.cpp
    src/lexer.cpp
    src/parser.cpp
    src/runtime.cpp
    src/interpreter.cpp
    src/market_sim.cpp
    src/jit_compiler.cpp
    src/signal_backend.cpp
    src/signal_program.cpp
    src/type_check.cpp
    src/multithread_eval.cpp
)

target_include_directories(signal_core PUBLIC src)

if (JITSE_ENABLE_LLVM)
  find_package(LLVM CONFIG QUIET)
  if (LLVM_FOUND)
    message(STATUS "LLVM found: ${LLVM_PACKAGE_VERSION}")
    target_compile_definitions(signal_core PUBLIC JITSE_HAS_LLVM=1)
    target_include_directories(signal_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if (LLVM_LIBRARY_DIRS)
      link_directories(${LLVM_LIBRARY_DIRS})
      target_link_directories(signal_core PRIVATE ${LLVM_LIBRARY_DIRS})
    endif()
    if (LLVM_LIBRARIES)
      target_link_libraries(signal_core PRIVATE ${LLVM_LIBRARIES})
    elseif (TARGET LLVM::LLVM)
      target_link_libraries(signal_core PRIVATE LLVM::LLVM)
    else()
      target_link_libraries(signal_core PRIVATE LLVM)
    endif()
    if (WIN32)
      target_link_libraries(signal_core PRIVATE ws2_32 z zstd)
    endif()
  else()
    message(WARNING "LLVM not found. Building without JIT backend.")
  endif()
endif()

add_executable(jit_signal_engine src/main.cpp)
target_link_libraries(jit_signal_engine PRIVATE signal_core)

enable_testing()
add_executable(lexer_test test/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE signal_core)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_smoke_test test/parser_smoke_test.cpp)
target_link_libraries(parser_smoke_test PRIVATE signal_core)
add_test(NAME parser_smoke_test COMMAND parser_smoke_test)

add_executable(interpreter_test test/interpreter_test.cpp)
target_link_libraries(interpreter_test PRIVATE signal_core)
add_test(NAME interpreter_test COMMAND interpreter_test)

add_executable(runtime_test test/runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE signal_core)
add_test(NAME runtime_test COMMAND runtime_test)

# P14 (Welford rolling stddev): gates that the O(1) incremental Welford
# path used by `RingStatsStddevSample` agrees with the two-pass
# long-double reference (kept exposed as
# `RingStatsStddevSampleTwoPassReference`) across benign,
# catastrophic-cancellation, and degenerate windows.
add_executable(welford_stddev_parity_test test/welford_stddev_parity_test.cpp)
target_link_libraries(welford_stddev_parity_test PRIVATE signal_core)
add_test(NAME welford_stddev_parity_test COMMAND welford_stddev_parity_test)

# spsc_ring_test: FIFO ordering, concurrent stress, no-alloc-on-hot-path
# discipline for the SPSC ring used in the live-ingest pipeline benchmark.
add_executable(spsc_ring_test test/spsc_ring_test.cpp)
target_link_libraries(spsc_ring_test PRIVATE signal_core)
add_test(NAME spsc_ring_test COMMAND spsc_ring_test)

add_executable(market_sim_test test/market_sim_test.cpp)
target_link_libraries(market_sim_test PRIVATE signal_core)
add_test(NAME market_sim_test COMMAND market_sim_test)

add_executable(jit_test test/jit_test.cpp)
target_link_libraries(jit_test PRIVATE signal_core)
add_test(NAME jit_test COMMAND jit_test)

add_executable(cse_load_dedup_test test/cse_load_dedup_test.cpp)
target_link_libraries(cse_load_dedup_test PRIVATE signal_core)
add_test(NAME cse_load_dedup_test COMMAND cse_load_dedup_test)
set_tests_properties(cse_load_dedup_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(signal_program_test test/signal_program_test.cpp)
target_link_libraries(signal_program_test PRIVATE signal_core)
add_test(NAME signal_program_test COMMAND signal_program_test)

add_executable(node_state_layout_test test/node_state_layout_test.cpp)
target_link_libraries(node_state_layout_test PRIVATE signal_core)
add_test(NAME node_state_layout_test COMMAND node_state_layout_test)

add_executable(hot_path_allocation_test test/hot_path_allocation_test.cpp)
target_link_libraries(hot_path_allocation_test PRIVATE signal_core)
add_test(NAME hot_path_allocation_test COMMAND hot_path_allocation_test)

add_executable(fuzz_parity_test test/fuzz_parity_test.cpp)
target_link_libraries(fuzz_parity_test PRIVATE signal_core)
add_test(NAME fuzz_parity_test COMMAND fuzz_parity_test)

add_executable(fuzz_parity_simd_test test/fuzz_parity_simd_test.cpp)
target_link_libraries(fuzz_parity_simd_test PRIVATE signal_core)
add_test(NAME fuzz_parity_simd_test COMMAND fuzz_parity_simd_test)

add_executable(fuzz_parity_multisymbol_test test/fuzz_parity_multisymbol_test.cpp)
target_link_libraries(fuzz_parity_multisymbol_test PRIVATE signal_core)
add_test(NAME fuzz_parity_multisymbol_test COMMAND fuzz_parity_multisymbol_test)

# P0: parity gate for the IR-lowered stateful operators (sma/ema/lag).
add_executable(stateful_lowering_parity_test test/stateful_lowering_parity_test.cpp)
target_link_libraries(stateful_lowering_parity_test PRIVATE signal_core)
add_test(NAME stateful_lowering_parity_test COMMAND stateful_lowering_parity_test)

# P1: parity gate for the tier-2 specialized (assume_warm) JIT.
add_executable(tiered_specialization_parity_test test/tiered_specialization_parity_test.cpp)
target_link_libraries(tiered_specialization_parity_test PRIVATE signal_core)
add_test(NAME tiered_specialization_parity_test COMMAND tiered_specialization_parity_test)

# P2: parity gate for cross-symbol vectorized JIT (lane_count = 4).
add_executable(vectorized_lanes_parity_test test/vectorized_lanes_parity_test.cpp)
target_link_libraries(vectorized_lanes_parity_test PRIVATE signal_core)
add_test(NAME vectorized_lanes_parity_test COMMAND vectorized_lanes_parity_test)

# P6.1: SourceLoc threading through Token + Expr; ParseError caret rendering.
add_executable(source_location_test test/source_location_test.cpp)
target_link_libraries(source_location_test PRIVATE signal_core)
add_test(NAME source_location_test COMMAND source_location_test)

# P6.2: AST constant folding + IR-equality between folded and baseline.
add_executable(constant_fold_test test/constant_fold_test.cpp)
target_link_libraries(constant_fold_test PRIVATE signal_core)
add_test(NAME constant_fold_test COMMAND constant_fold_test)

# P6.3: bool-vs-number type system.
add_executable(type_check_test test/type_check_test.cpp)
target_link_libraries(type_check_test PRIVATE signal_core)
add_test(NAME type_check_test COMMAND type_check_test)

# P7: parity gate for rolling_corr, rolling_beta, kalman1d.
add_executable(rolling_pair_kalman_parity_test test/rolling_pair_kalman_parity_test.cpp)
target_link_libraries(rolling_pair_kalman_parity_test PRIVATE signal_core)
add_test(NAME rolling_pair_kalman_parity_test COMMAND rolling_pair_kalman_parity_test)

# P7: differential oracle vs. naive two-pass / textbook Kalman references.
add_executable(rolling_pair_kalman_oracle_test test/rolling_pair_kalman_oracle_test.cpp)
target_link_libraries(rolling_pair_kalman_oracle_test PRIVATE signal_core)
add_test(NAME rolling_pair_kalman_oracle_test COMMAND rolling_pair_kalman_oracle_test)

# P15: jitse-fmt + jitse-lint subcommands.
#
# jitse_fmt is the canonical pretty-printer for the DSL (parser AST
# round-trip via `dsl_formatter`). Idempotent and stable.
#
# jitse_lint runs the full frontend (parse, type-check, fold, inline,
# allocate node-ids, bind symbol-ids) and emits caret-style
# diagnostics on any finding. Exits 0 on a clean program.
add_executable(jitse_fmt cli/jitse_fmt.cpp)
target_link_libraries(jitse_fmt PRIVATE signal_core)

add_executable(jitse_lint cli/jitse_lint.cpp)
target_link_libraries(jitse_lint PRIVATE signal_core)

add_executable(jitse_calibrate cli/jitse_calibrate.cpp)
target_link_libraries(jitse_calibrate PRIVATE signal_core)

# Round-trip test: every .sig in examples/ must parse, format,
# reparse to a structurally-equal AST, AND the formatter must be
# idempotent (fmt(fmt(x)) == fmt(x)). The lint test asserts that
# every .sig in examples/ is clean by the linter's standards and
# that a deliberately-broken program exits 1.
add_executable(dsl_formatter_roundtrip_test test/dsl_formatter_roundtrip_test.cpp)
target_link_libraries(dsl_formatter_roundtrip_test PRIVATE signal_core)
add_test(NAME dsl_formatter_roundtrip_test COMMAND dsl_formatter_roundtrip_test)

# P13: persistent JIT module cache. Hashes program AST + lowering +
# lane count + AVX2 + LLVM version, persists the post-O2 bitcode on
# disk, and skips IR + O2 on a cache hit. Test asserts correctness
# (bit-equal traces vs no-cache reference), cache hit detection, a
# material speedup on the warm compile, and that distinct programs
# leave distinct .bc files (no key collisions on this workload).
add_executable(jit_module_cache_test test/jit_module_cache_test.cpp)
target_link_libraries(jit_module_cache_test PRIVATE signal_core)
add_test(NAME jit_module_cache_test COMMAND jit_module_cache_test)

# P11: gate that fixes the conditional+stateful inliner divergence
# uncovered while landing P10. The new AllocateNodeIds dedup pass +
# JIT/interpreter caches ensure structurally-equal stateful subtrees
# (produced by InlineSignalDependencies cloning the same body twice)
# share state and emit exactly one runtime call per tick.
add_executable(stateful_subtree_dedup_test test/stateful_subtree_dedup_test.cpp)
target_link_libraries(stateful_subtree_dedup_test PRIVATE signal_core)
add_test(NAME stateful_subtree_dedup_test COMMAND stateful_subtree_dedup_test)

# P10: parity gate for vectorized stateful operators via per-lane
# scalarized fan-out. Compiles each test program in scalar and 4-lane
# vectorized modes and asserts bit-equal output across 1500 ticks of
# four independent price streams.
add_executable(vectorized_stateful_parity_test test/vectorized_stateful_parity_test.cpp)
target_link_libraries(vectorized_stateful_parity_test PRIVATE signal_core)
add_test(NAME vectorized_stateful_parity_test COMMAND vectorized_stateful_parity_test)

# P9: post-O2 IR + captured-asm content assertions ("did the optimizer
# do the work we wanted?"). Asserts default lowering keeps runtime
# sma/ema/lag calls AND full lowering eliminates them, and that the
# captured asm contains real x86 FP arithmetic mnemonics.
add_executable(optimization_evidence_test test/optimization_evidence_test.cpp)
target_link_libraries(optimization_evidence_test PRIVATE signal_core)
add_test(NAME optimization_evidence_test
         COMMAND optimization_evidence_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# P8: parser and runtime fuzz harnesses. Two build modes:
#   - JITSE_BUILD_FUZZERS=OFF (default): standalone corpus-driver
#     executables that walk fuzz/corpus and run each input through
#     LLVMFuzzerTestOneInput. Works with any compiler; serves as a
#     CI smoke gate / regression test for previously-found inputs.
#   - JITSE_BUILD_FUZZERS=ON: clang + -fsanitize=fuzzer harnesses for
#     real libFuzzer campaigns. Standalone smoke tests skipped.
if (JITSE_BUILD_FUZZERS)
  if (NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR "JITSE_BUILD_FUZZERS requires Clang; reconfigure with CC=clang CXX=clang++.")
  endif()
  add_executable(parser_fuzzer fuzz/parser_fuzzer.cpp)
  target_compile_options(parser_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(parser_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(parser_fuzzer PRIVATE signal_core)

  add_executable(runtime_fuzzer fuzz/runtime_fuzzer.cpp)
  target_compile_options(runtime_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(runtime_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(runtime_fuzzer PRIVATE signal_core)
else()
  # Standalone corpus drivers. JITSE_FUZZ_LINK_DRIVER enables the
  # `int main()` epilogue inside each fuzz source.
  add_executable(parser_fuzzer_smoke fuzz/parser_fuzzer.cpp)
  target_compile_definitions(parser_fuzzer_smoke PRIVATE JITSE_FUZZ_LINK_DRIVER=1)
  target_link_libraries(parser_fuzzer_smoke PRIVATE signal_core)
  add_test(NAME parser_fuzzer_smoke
           COMMAND parser_fuzzer_smoke ${CMAKE_SOURCE_DIR}/fuzz/corpus)

  add_executable(runtime_fuzzer_smoke fuzz/runtime_fuzzer.cpp)
  target_compile_definitions(runtime_fuzzer_smoke PRIVATE JITSE_FUZZ_LINK_DRIVER=1)
  target_link_libraries(runtime_fuzzer_smoke PRIVATE signal_core)
  add_test(NAME runtime_fuzzer_smoke
           COMMAND runtime_fuzzer_smoke ${CMAKE_SOURCE_DIR}/fuzz/corpus)
endif()

if (WIN32 AND LLVM_FOUND)
  if (DEFINED LLVM_TOOLS_BINARY_DIR)
    set(_jitse_test_path "PATH=${LLVM_TOOLS_BINARY_DIR};$ENV{PATH}")
    set_tests_properties(jit_test PROPERTIES ENVIRONMENT "${_jitse_test_path}")
    set_tests_properties(fuzz_parity_test PROPERTIES ENVIRONMENT "${_jitse_test_path}")
  endif()
endif()

add_executable(signal_benchmark bench/signal_benchmark.cpp bench/hw_reference.cpp)
target_link_libraries(signal_benchmark PRIVATE signal_core)

add_executable(scaling_benchmark bench/scaling_benchmark.cpp)
target_link_libraries(scaling_benchmark PRIVATE signal_core)

add_executable(program_benchmark bench/program_benchmark.cpp)
target_link_libraries(program_benchmark PRIVATE signal_core)

add_executable(autodiff_benchmark bench/autodiff_benchmark.cpp)
target_link_libraries(autodiff_benchmark PRIVATE signal_core)

add_executable(simd_benchmark bench/simd_benchmark.cpp)
target_link_libraries(simd_benchmark PRIVATE signal_core)

add_executable(stateful_lowering_ir_diff bench/stateful_lowering_ir_diff.cpp)
target_link_libraries(stateful_lowering_ir_diff PRIVATE signal_core)

# P2: cross-symbol vectorization throughput comparison.
add_executable(cross_symbol_benchmark bench/cross_symbol_benchmark.cpp)
target_link_libraries(cross_symbol_benchmark PRIVATE signal_core)

# P12: composition benchmark for cross-symbol vectorization (P2/P10) x
# multi-threading (P5). Measures the multiplicative speedup of
# {scalar+1T, scalar+NT, vec+1T, vec+NT} on stateless and stateful
# programs and emits a Markdown artifact comparable to the rest of the
# roadmap. The throughput numbers are best-of-N (consistent with the
# other roadmap benchmarks); the per-run wall-times and the median are
# both reported so the reader can sanity-check.
add_executable(vec_thread_composition_benchmark bench/vec_thread_composition_benchmark.cpp)
target_link_libraries(vec_thread_composition_benchmark PRIVATE signal_core)

# P3: in-process SIGPROF sampling profile of the JIT hot loop. Compares
# lowering=none vs lowering=all on filtered_momentum.sig to show the
# jit_rt_* helpers vanishing from the hot path under P0.
add_executable(runtime_call_profile
  bench/runtime_call_profile.cpp
  bench/sampling_profiler.cpp)
target_link_libraries(runtime_call_profile PRIVATE signal_core)
if(UNIX AND NOT APPLE)
  # dladdr requires -ldl on Linux glibc.
  target_link_libraries(runtime_call_profile PRIVATE dl)
  # ENABLE_EXPORTS=ON (equivalent to -rdynamic / --export-dynamic) puts
  # every static symbol from the executable into the dynamic symbol
  # table, so dladdr can resolve PCs landing in `jit_rt_*` runtime
  # helpers back to their names. Without this every helper IP resolves
  # to `[unknown]` and the profile is useless.
  set_target_properties(runtime_call_profile PROPERTIES ENABLE_EXPORTS ON)
endif()

# P3 smoke gate: asserts P0-lowered helpers drop to ~0% sample share.
add_executable(runtime_call_profile_test
  test/runtime_call_profile_test.cpp
  bench/sampling_profiler.cpp)
target_include_directories(runtime_call_profile_test PRIVATE bench)
target_link_libraries(runtime_call_profile_test PRIVATE signal_core)
if(UNIX AND NOT APPLE)
  target_link_libraries(runtime_call_profile_test PRIVATE dl)
  set_target_properties(runtime_call_profile_test PROPERTIES ENABLE_EXPORTS ON)
endif()
if (JITSE_ENABLE_LLVM AND LLVM_FOUND)
  add_test(NAME runtime_call_profile_test COMMAND runtime_call_profile_test)
endif()

# P4: latency-distribution artifact (CO-aware per-call histograms).
add_library(latency_histogram_lib STATIC bench/latency_histogram.cpp)
target_include_directories(latency_histogram_lib PUBLIC bench)

add_executable(latency_bench bench/latency_bench.cpp)
target_link_libraries(latency_bench PRIVATE signal_core latency_histogram_lib)

# Live-ingest pipeline benchmark: SPSC ring + JIT eval, measures
# enqueue-to-signal-output p50/p99/p999 latency across cores. See
# `src/spsc_ring.h` for the lock-free ring; gated for correctness by
# `spsc_ring_test`. Linux-only because the affinity helpers are
# pthread-based.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(spsc_jit_pipeline_bench bench/spsc_jit_pipeline_bench.cpp)
  target_link_libraries(spsc_jit_pipeline_bench PRIVATE signal_core latency_histogram_lib)
endif()

add_executable(latency_histogram_test test/latency_histogram_test.cpp)
target_link_libraries(latency_histogram_test PRIVATE latency_histogram_lib)
add_test(NAME latency_histogram_test COMMAND latency_histogram_test)

# P5: compile-vs-runtime crossover bench (uses JitCompiler::LastCompileTimings).
add_executable(compile_runtime_crossover bench/compile_runtime_crossover.cpp)
target_link_libraries(compile_runtime_crossover PRIVATE signal_core)

add_executable(compile_timings_test test/compile_timings_test.cpp)
target_link_libraries(compile_timings_test PRIVATE signal_core)
add_test(NAME compile_timings_test COMMAND compile_timings_test)

add_executable(multisymbol_benchmark bench/multisymbol_benchmark.cpp)
target_link_libraries(multisymbol_benchmark PRIVATE signal_core)

add_executable(multithread_scaling_benchmark bench/multithread_scaling_benchmark.cpp)
target_link_libraries(multithread_scaling_benchmark PRIVATE signal_core)

add_executable(multithread_equivalence_test test/multithread_equivalence_test.cpp)
target_link_libraries(multithread_equivalence_test PRIVATE signal_core)
add_test(NAME multithread_equivalence_test COMMAND multithread_equivalence_test)
set_tests_properties(multithread_equivalence_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(gradient_parity_test test/gradient_parity_test.cpp)
target_link_libraries(gradient_parity_test PRIVATE signal_core)
add_test(NAME gradient_parity_test COMMAND gradient_parity_test)

if (JITSE_ENABLE_LLVM AND LLVM_FOUND)
  add_test(
      NAME calibration_smoke_test
      COMMAND jitse_calibrate
        --signal-file ${CMAKE_SOURCE_DIR}/examples/calibration_fit_target.sig
        --ticks-csv ${CMAKE_SOURCE_DIR}/test/fixtures/calibration_ticks.csv
        --output-signal pred
        --out-dir ${CMAKE_CURRENT_BINARY_DIR}/autodiff_smoke
        --artifact-stem calibration_smoke
        --iterations 80
        --learning-rate 0.05
        --beta1 0.9
        --beta2 0.999
        --epsilon 1e-8
        --min-improvement 0.02
  )
endif()

set(JITSE_MDH_ROOT "/mnt/c/Users/bhask/Documents/PROJECTS/market-data-handler" CACHE PATH "Path to market-data-handler repo")
if (EXISTS "${JITSE_MDH_ROOT}/include/mf/journal/journal_reader.hpp")
  set(JITSE_MDH_JOURNAL_SOURCES
      ${JITSE_MDH_ROOT}/src/core/crc32.cpp
      ${JITSE_MDH_ROOT}/src/journal/journal_reader.cpp
  )

  add_executable(backtest_runner
      examples/backtest_runner.cpp
      ${JITSE_MDH_JOURNAL_SOURCES}
  )
  target_include_directories(backtest_runner PRIVATE src ${JITSE_MDH_ROOT}/include)
  target_link_libraries(backtest_runner PRIVATE signal_core)

  add_executable(journal_to_csv
      examples/journal_to_csv.cpp
      ${JITSE_MDH_JOURNAL_SOURCES}
  )
  target_include_directories(journal_to_csv PRIVATE src ${JITSE_MDH_ROOT}/include)
  target_link_libraries(journal_to_csv PRIVATE signal_core)

  add_executable(backtest_determinism_test test/backtest_determinism_test.cpp)
  add_dependencies(backtest_determinism_test backtest_runner)
  if (JITSE_ENABLE_LLVM AND LLVM_FOUND)
    add_test(NAME backtest_determinism_test COMMAND backtest_determinism_test)
  endif()

  add_test(
      NAME differential_oracle_test
      # Deterministic AAPL fixture provides repeated signal-bearing events so this is
      # a strict oracle gate instead of the old bootstrap-only pipeline smoke test.
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/bench/run_diff_oracle.sh --fixture synthetic_aapl --out-root ${CMAKE_CURRENT_BINARY_DIR}/diff_test/ctest_fixture --run-id ctest_diff_test --min-within-rate 0.999
  )
  set_tests_properties(differential_oracle_test PROPERTIES TIMEOUT 60)
endif()
