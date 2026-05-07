#include <cassert>
#include <cmath>
#include <limits>
#include <string>

#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

int main() {
  auto Prepare = [](jitse::SignalDef& def, jitse::SignalContext& local_ctx) {
    jitse::AllocateNodeIds(def);
    jitse::PrewarmSignalContext(local_ctx, def);
  };

  jitse::SymbolTable symbols;
  const std::size_t aapl_id = symbols.RegisterOrGetId("AAPL");
  const std::size_t msft_id = symbols.RegisterOrGetId("MSFT");

  jitse::MarketState market;
  jitse::SignalContext ctx;
  market.instruments[aapl_id].bid = 100.0;
  market.instruments[aapl_id].ask = 102.0;  // mid = 101
  market.instruments[msft_id].bid = 49.0;
  market.instruments[msft_id].ask = 51.0;   // mid = 50

  {
    const std::string src = "signal spread = mid(AAPL) - mid(MSFT)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, ctx);
    jitse::Interpreter interp(symbols);
    const double v = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v - 51.0) < 1e-12);
  }

  {
    const std::string src = "signal expr = 1 + 2 * 3";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, ctx);
    jitse::Interpreter interp(symbols);
    const double v = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v - 7.0) < 1e-12);
  }
  {
    const std::string src = "signal capped = if mid(AAPL) > mid(MSFT) then 0 else mid(AAPL) - mid(MSFT)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, ctx);
    jitse::Interpreter interp(symbols);
    const double v = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v - 0.0) < 1e-12);
  }
  {
    market.instruments[aapl_id].bid = 10.0;
    market.instruments[aapl_id].ask = 12.0;  // mid 11
    market.instruments[msft_id].bid = 49.0;
    market.instruments[msft_id].ask = 51.0;  // mid 50
    const std::string src = "signal capped = if mid(AAPL) > mid(MSFT) then 1 else -1";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, ctx);
    jitse::Interpreter interp(symbols);
    const double v = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v + 1.0) < 1e-12);
  }
  {
    market.instruments[aapl_id].bid = 100.0;
    market.instruments[aapl_id].ask = 100.0;  // mid 100
    const std::string src = "signal e = ema(mid(AAPL), 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, ctx);
    jitse::Interpreter interp(symbols);

    // First sample initializes EMA directly to x.
    double v1 = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v1 - 100.0) < 1e-12);

    // Same input keeps EMA stable at 100.
    double v2 = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v2 - 100.0) < 1e-12);

    // For period=3, alpha=0.5. New x=110 -> ema=0.5*110 + 0.5*100 = 105.
    market.instruments[aapl_id].bid = 110.0;
    market.instruments[aapl_id].ask = 110.0;
    double v3 = interp.Evaluate(def, market, ctx);
    assert(std::fabs(v3 - 105.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal s = sma(mid(AAPL), 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);

    market.instruments[aapl_id].bid = 9.0;
    market.instruments[aapl_id].ask = 9.0;
    double v1 = interp.Evaluate(def, market, local_ctx);
    assert(std::isnan(v1));

    market.instruments[aapl_id].bid = 12.0;
    market.instruments[aapl_id].ask = 12.0;
    double v2 = interp.Evaluate(def, market, local_ctx);
    assert(std::isnan(v2));

    market.instruments[aapl_id].bid = 15.0;
    market.instruments[aapl_id].ask = 15.0;
    double v3 = interp.Evaluate(def, market, local_ctx);
    assert(std::fabs(v3 - 12.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal r = rolling_std(mid(AAPL), 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);

    market.instruments[aapl_id].bid = 1.0;
    market.instruments[aapl_id].ask = 1.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));

    market.instruments[aapl_id].bid = 2.0;
    market.instruments[aapl_id].ask = 2.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));

    market.instruments[aapl_id].bid = 3.0;
    market.instruments[aapl_id].ask = 3.0;
    double v = interp.Evaluate(def, market, local_ctx);
    // sample stddev of [1,2,3] is 1.
    assert(std::fabs(v - 1.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal f = abs(-3) + sqrt(9) + log(1)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);
    double v = interp.Evaluate(def, market, local_ctx);
    assert(std::fabs(v - 6.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal m = rolling_min(mid(AAPL), 3) + rolling_max(mid(AAPL), 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);

    market.instruments[aapl_id].bid = 5.0;
    market.instruments[aapl_id].ask = 5.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 2.0;
    market.instruments[aapl_id].ask = 2.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 8.0;
    market.instruments[aapl_id].ask = 8.0;
    double v = interp.Evaluate(def, market, local_ctx);
    // window [5,2,8]: min=2, max=8, sum=10
    assert(std::fabs(v - 10.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal z = zscore(mid(AAPL), 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);

    market.instruments[aapl_id].bid = 1.0;
    market.instruments[aapl_id].ask = 1.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 2.0;
    market.instruments[aapl_id].ask = 2.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 3.0;
    market.instruments[aapl_id].ask = 3.0;
    const double z = interp.Evaluate(def, market, local_ctx);
    assert(std::fabs(z - 1.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal v = vwap(AAPL, 3)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);

    market.instruments[aapl_id].bid = 9.0;
    market.instruments[aapl_id].ask = 11.0;   // mid 10
    market.instruments[aapl_id].volume = 2.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));

    market.instruments[aapl_id].bid = 19.0;
    market.instruments[aapl_id].ask = 21.0;   // mid 20
    market.instruments[aapl_id].volume = 1.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));

    market.instruments[aapl_id].bid = 29.0;
    market.instruments[aapl_id].ask = 31.0;   // mid 30
    market.instruments[aapl_id].volume = 1.0;
    const double v = interp.Evaluate(def, market, local_ctx);
    // (10*2 + 20*1 + 30*1) / (2 + 1 + 1) = 17.5
    assert(std::fabs(v - 17.5) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal l = lag(mid(AAPL), 2)";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);
    market.instruments[aapl_id].bid = 10.0;
    market.instruments[aapl_id].ask = 10.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 20.0;
    market.instruments[aapl_id].ask = 20.0;
    assert(std::isnan(interp.Evaluate(def, market, local_ctx)));
    market.instruments[aapl_id].bid = 30.0;
    market.instruments[aapl_id].ask = 30.0;
    const double v = interp.Evaluate(def, market, local_ctx);
    assert(std::fabs(v - 10.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal c = cross_above(mid(AAPL), mid(MSFT))";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);
    market.instruments[aapl_id].bid = 10.0;
    market.instruments[aapl_id].ask = 10.0;
    market.instruments[msft_id].bid = 20.0;
    market.instruments[msft_id].ask = 20.0;
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
    market.instruments[aapl_id].bid = 25.0;
    market.instruments[aapl_id].ask = 25.0;
    market.instruments[msft_id].bid = 20.0;
    market.instruments[msft_id].ask = 20.0;
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 1.0) < 1e-12);
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    const std::string src = "signal c = cross_below(mid(AAPL), mid(MSFT))";
    jitse::Lexer lexer(src);
    jitse::Parser parser(lexer.Tokenize());
    jitse::SignalDef def = parser.ParseSignalDef();
    Prepare(def, local_ctx);
    jitse::Interpreter interp(symbols);
    market.instruments[aapl_id].bid = 30.0;
    market.instruments[aapl_id].ask = 30.0;
    market.instruments[msft_id].bid = 20.0;
    market.instruments[msft_id].ask = 20.0;
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
    market.instruments[aapl_id].bid = 15.0;
    market.instruments[aapl_id].ask = 15.0;
    market.instruments[msft_id].bid = 20.0;
    market.instruments[msft_id].ask = 20.0;
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 1.0) < 1e-12);
    assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
  }
  {
    jitse::SignalContext local_ctx;
    {
      const std::string src = "signal a = 1.0 && 1.0";
      jitse::Lexer lexer(src);
      jitse::Parser parser(lexer.Tokenize());
      jitse::SignalDef def = parser.ParseSignalDef();
      Prepare(def, local_ctx);
      jitse::Interpreter interp(symbols);
      assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 1.0) < 1e-12);
    }
    {
      const std::string src = "signal b = 1.0 && 0.0";
      jitse::Lexer lexer(src);
      jitse::Parser parser(lexer.Tokenize());
      jitse::SignalDef def = parser.ParseSignalDef();
      Prepare(def, local_ctx);
      jitse::Interpreter interp(symbols);
      assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
    }
    {
      const std::string src = "signal c = 0.0 || 1.0";
      jitse::Lexer lexer(src);
      jitse::Parser parser(lexer.Tokenize());
      jitse::SignalDef def = parser.ParseSignalDef();
      Prepare(def, local_ctx);
      jitse::Interpreter interp(symbols);
      assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 1.0) < 1e-12);
    }
    {
      const std::string src = "signal d = 1.0 != 2.0";
      jitse::Lexer lexer(src);
      jitse::Parser parser(lexer.Tokenize());
      jitse::SignalDef def = parser.ParseSignalDef();
      Prepare(def, local_ctx);
      jitse::Interpreter interp(symbols);
      assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 1.0) < 1e-12);
    }
    {
      const std::string src = "signal e = 1.0 != 1.0";
      jitse::Lexer lexer(src);
      jitse::Parser parser(lexer.Tokenize());
      jitse::SignalDef def = parser.ParseSignalDef();
      Prepare(def, local_ctx);
      jitse::Interpreter interp(symbols);
      assert(std::fabs(interp.Evaluate(def, market, local_ctx) - 0.0) < 1e-12);
    }
  }

  return 0;
}
