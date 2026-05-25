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

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_library(signal_core
    src/ast_utils.cpp
    src/ast_clone.cpp
    src/ast_printer.cpp
    src/lexer.cpp
    src/parser.cpp
    src/runtime.cpp
    src/interpreter.cpp
    src/market_sim.cpp
    src/jit_compiler.cpp
    src/signal_backend.cpp
    src/signal_program.cpp
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

if (WIN32 AND LLVM_FOUND)
  if (DEFINED LLVM_TOOLS_BINARY_DIR)
    set(_jitse_test_path "PATH=${LLVM_TOOLS_BINARY_DIR};$ENV{PATH}")
    set_tests_properties(jit_test PROPERTIES ENVIRONMENT "${_jitse_test_path}")
    set_tests_properties(fuzz_parity_test PROPERTIES ENVIRONMENT "${_jitse_test_path}")
  endif()
endif()

add_executable(signal_benchmark bench/signal_benchmark.cpp)
target_link_libraries(signal_benchmark PRIVATE signal_core)

add_executable(scaling_benchmark bench/scaling_benchmark.cpp)
target_link_libraries(scaling_benchmark PRIVATE signal_core)

add_executable(program_benchmark bench/program_benchmark.cpp)
target_link_libraries(program_benchmark PRIVATE signal_core)

add_executable(simd_benchmark bench/simd_benchmark.cpp)
target_link_libraries(simd_benchmark PRIVATE signal_core)

add_executable(stateful_lowering_ir_diff bench/stateful_lowering_ir_diff.cpp)
target_link_libraries(stateful_lowering_ir_diff PRIVATE signal_core)

# P2: cross-symbol vectorization throughput comparison.
add_executable(cross_symbol_benchmark bench/cross_symbol_benchmark.cpp)
target_link_libraries(cross_symbol_benchmark PRIVATE signal_core)

add_executable(multisymbol_benchmark bench/multisymbol_benchmark.cpp)
target_link_libraries(multisymbol_benchmark PRIVATE signal_core)

add_executable(multithread_scaling_benchmark bench/multithread_scaling_benchmark.cpp)
target_link_libraries(multithread_scaling_benchmark PRIVATE signal_core)

add_executable(multithread_equivalence_test test/multithread_equivalence_test.cpp)
target_link_libraries(multithread_equivalence_test PRIVATE signal_core)
add_test(NAME multithread_equivalence_test COMMAND multithread_equivalence_test)
set_tests_properties(multithread_equivalence_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
  add_test(NAME backtest_determinism_test COMMAND backtest_determinism_test)

  add_test(
      NAME differential_oracle_test
      # Deterministic AAPL fixture provides repeated signal-bearing events so this is
      # a strict oracle gate instead of the old bootstrap-only pipeline smoke test.
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/bench/run_diff_oracle.sh --fixture synthetic_aapl --out-root ${CMAKE_CURRENT_BINARY_DIR}/diff_test/ctest_fixture --run-id ctest_diff_test --min-within-rate 0.999
  )
  set_tests_properties(differential_oracle_test PROPERTIES TIMEOUT 60)
endif()
