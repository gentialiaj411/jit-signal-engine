#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "signal_program.h"

int main() {
  // --- dependency inlining + eval ---
  const std::string src =
      "signal base = mid(AAPL) - mid(MSFT)\n"
      "signal z = abs(base)\n";

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  assert(parsed.size() == 2);
  std::vector<jitse::SignalDef> inlined = jitse::InlineSignalDependencies(parsed);
  assert(inlined.size() == 2);

  jitse::SymbolTable symbols;
  for (const auto& t : jitse::CollectTickerSymbols(inlined.back())) symbols.RegisterOrGetId(t);
  const std::size_t aapl = symbols.LookupId("AAPL");
  const std::size_t msft = symbols.LookupId("MSFT");

  jitse::MarketState market;
  market.instruments[aapl].bid = 101.0;
  market.instruments[aapl].ask = 103.0;  // mid 102
  market.instruments[msft].bid = 98.0;
  market.instruments[msft].ask = 100.0;  // mid 99

  jitse::SignalContext ctx;
  jitse::Interpreter interp(symbols);
  const double out = interp.Evaluate(inlined.back(), market, ctx);
  assert(std::fabs(out - 3.0) < 1e-12);

  // --- cycle detection ---
  // a references b and b references a; InlineSignalDependencies must throw.
  const std::string cycle_src =
      "signal a = b * 2.0\n"
      "signal b = a * 2.0\n";
  std::vector<jitse::SignalDef> cycle_parsed = jitse::ParseSignalProgram(cycle_src);
  bool caught_cycle = false;
  try {
    jitse::InlineSignalDependencies(cycle_parsed);
  } catch (const std::runtime_error& e) {
    const std::string msg = e.what();
    caught_cycle = (msg.find("Cycle") != std::string::npos || msg.find("cycle") != std::string::npos);
  }
  assert(caught_cycle && "InlineSignalDependencies must throw on cyclic signal graph");

  // --- duplicate signal name detection ---
  const std::string dup_src =
      "signal x = mid(AAPL)\n"
      "signal x = mid(MSFT)\n";
  bool caught_dup = false;
  try {
    std::vector<jitse::SignalDef> dup_parsed = jitse::ParseSignalProgram(dup_src);
    jitse::InlineSignalDependencies(dup_parsed);
  } catch (const std::runtime_error& e) {
    caught_dup = true;
    (void)e;
  }
  assert(caught_dup && "duplicate signal names must be rejected");

  return 0;
}
