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
)

target_include_directories(signal_core PUBLIC src)

if (JITSE_ENABLE_LLVM)
  find_package(LLVM 17 CONFIG QUIET)
  if (LLVM_FOUND)
    message(STATUS "LLVM found: ${LLVM_PACKAGE_VERSION}")
    target_compile_definitions(signal_core PUBLIC JITSE_HAS_LLVM=1)
    target_include_directories(signal_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(signal_core PRIVATE LLVM)
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

add_executable(signal_program_test test/signal_program_test.cpp)
target_link_libraries(signal_program_test PRIVATE signal_core)
add_test(NAME signal_program_test COMMAND signal_program_test)

add_executable(fuzz_parity_test test/fuzz_parity_test.cpp)
target_link_libraries(fuzz_parity_test PRIVATE signal_core)
add_test(NAME fuzz_parity_test COMMAND fuzz_parity_test)

add_executable(signal_benchmark bench/signal_benchmark.cpp)
target_link_libraries(signal_benchmark PRIVATE signal_core)

add_executable(scaling_benchmark bench/scaling_benchmark.cpp)
target_link_libraries(scaling_benchmark PRIVATE signal_core)

add_executable(program_benchmark bench/program_benchmark.cpp)
target_link_libraries(program_benchmark PRIVATE signal_core)
