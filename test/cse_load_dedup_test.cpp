#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "parser.h"
#include "signal_program.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int CountMarketBidAskLoads(const std::string& ir) {
  static const std::regex kLineRe(R"(^\s*%((?:mid_)?(?:bid|ask))\d* = load double)");
  int count = 0;
  std::istringstream in(ir);
  std::string line;
  while (std::getline(in, line)) {
    if (std::regex_search(line, kLineRe)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main() {
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) {
    std::cout << "cse_load_dedup_test=skip (no LLVM)\n";
    return 0;
  }

  try {
    std::vector<jitse::SignalDef> parsed =
        jitse::ParseSignalProgram(ReadFile("examples/filtered_momentum.sig"));
    std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
    jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) {
      symbols.RegisterOrGetId(t);
    }
  }
  for (auto& s : signals) {
    jitse::BindSymbolIds(s, symbols);
  }

  if (!jit.CompileProgram(signals, symbols)) {
    std::cerr << "CompileProgram failed: " << jit.LastError() << "\n";
    return 1;
  }

  const int pre = CountMarketBidAskLoads(jit.LastIRPreOpt());
  const int post = CountMarketBidAskLoads(jit.LastIRPostOpt());
  std::cout << "market_bid_ask_loads_pre_opt=" << pre << "\n";
  std::cout << "market_bid_ask_loads_post_opt=" << post << "\n";

  // filtered_momentum uses mid(AAPL) in three stateful inputs; memoization emits one bid+ask pair pre-O2.
  constexpr int kExpectedPreLoads = 2;
  if (pre != kExpectedPreLoads || post < 1 || post > kExpectedPreLoads) {
    std::cerr << "Expected pre=" << kExpectedPreLoads << " and post in [1," << kExpectedPreLoads
              << "], got pre=" << pre << " post=" << post << "\n";
    return 1;
  }
  std::cout << "cse_load_dedup_test=pass\n";
  return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
