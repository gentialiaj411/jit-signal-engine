// P8: libFuzzer harness for the DSL parser.
//
// The harness feeds raw bytes to `ParseSignalProgram` and asserts no
// crash, no hang, no undefined behavior under -fsanitize=address,
// undefined. The DSL parser is the only piece of the engine that
// accepts untrusted input (signal source files); this fuzzer is the
// guard against parser-level memory safety bugs.
//
// Build modes:
//
//   1. libFuzzer mode (clang + -fsanitize=fuzzer): provides
//      LLVMFuzzerTestOneInput; the libFuzzer driver calls it with
//      random byte sequences derived from the seed corpus.
//
//   2. Standalone corpus-driver mode (any compiler, JITSE_FUZZ_LINK_DRIVER
//      defined): provides a `main` that iterates a directory of
//      corpus files and runs each through the same TestOneInput. This
//      lets CI run a "fuzzer smoke gate" without requiring clang or
//      the libFuzzer runtime, and gives us a deterministic regression
//      test for previously-found inputs.
//
// What the harness checks:
//   - ParseSignalProgram terminates without UB/ASan failure.
//   - The post-parse pipeline (type check + constant fold, both run
//     inside ParseSignalProgram) accepts or rejects each input via a
//     std::exception (which is fine).
//
// What it does NOT check:
//   - Semantic correctness of arbitrary AST. That's the runtime
//     fuzzer's job.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "signal_program.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // libFuzzer reuses memory across calls; copy into a std::string.
  // Cap at 64KB to keep individual inputs from melting the parser's
  // O(n) tokenizer with adversarial inputs (we want bug-finding, not
  // a DoS benchmark).
  if (size > 65536) size = 65536;
  std::string src(reinterpret_cast<const char*>(data), size);
  try {
    auto signals = jitse::ParseSignalProgram(src);
    (void)signals;
  } catch (const std::exception&) {
    // Any std::exception is a normal parse rejection. The crash gate
    // is "no UB / no ASan complaint / no abort", which libFuzzer and
    // the sanitizers handle.
  } catch (...) {
    // Non-std::exception throws are a bug. Crash so libFuzzer logs it.
    std::abort();
  }
  return 0;
}

#ifdef JITSE_FUZZ_LINK_DRIVER
// Standalone corpus driver. Used when libFuzzer is unavailable
// (compiler != clang, or CMake didn't enable JITSE_BUILD_FUZZERS).
// Walks the corpus directory passed as argv[1] (or fuzz/corpus
// relative to cwd if absent) and runs each file through the harness.
//
// Exits 0 on success; any abort/crash inside TestOneInput will
// terminate via signal, which is what we want a CI smoke gate to
// catch.
int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  fs::path corpus_dir = (argc >= 2) ? fs::path(argv[1]) : fs::path("fuzz/corpus");
  if (!fs::exists(corpus_dir) || !fs::is_directory(corpus_dir)) {
    std::cerr << "parser_fuzzer: corpus directory not found: " << corpus_dir << "\n";
    return 2;
  }
  std::size_t n_inputs = 0;
  for (const auto& entry : fs::directory_iterator(corpus_dir)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string contents = buf.str();
    const auto* data = reinterpret_cast<const std::uint8_t*>(contents.data());
    (void)LLVMFuzzerTestOneInput(data, contents.size());
    ++n_inputs;
  }
  std::cout << "parser_fuzzer: drove " << n_inputs
            << " corpus inputs through LLVMFuzzerTestOneInput, no crashes\n";
  return 0;
}
#endif
