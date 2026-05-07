#include <cassert>
#include <cmath>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "signal_program.h"

int main() {
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
  return 0;
}

