#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "lexer.h"
#include "market_sim.h"
#include "parser.h"
#include "runtime.h"
#include "signal_program.h"

int main() {
  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) {
    // Acceptable in environments without LLVM installed.
    std::cout << "jit_available=false\n";
    return 0;
  }
  std::cout << "jit_available=true\n";

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");
  symbols.RegisterOrGetId("MSFT");

  jitse::Lexer lexer("signal x = mid(AAPL) + 1");
  jitse::Parser parser(lexer.Tokenize());
  jitse::SignalDef def = parser.ParseSignalDef();

  const bool ok = jit.Compile(def, symbols);
  assert(ok);
  jitse::JitCompiler::JitFn fn = jit.GetFunction();
  assert(fn != nullptr);
  std::cout << "jit_mode=enabled\n";

  jitse::MarketState market;
  const std::size_t aapl = symbols.LookupId("AAPL");
  market.instruments[aapl].bid = 100.0;
  market.instruments[aapl].ask = 102.0;
  jitse::SignalContext ctx;
  const double out = fn(&market, &ctx);

  jitse::Interpreter interp(symbols);
  jitse::SignalContext interp_ctx;
  const double expected = interp.Evaluate(def, market, interp_ctx);
  assert(std::fabs(out - expected) < 1e-9);

  // Regression test: NaN condition semantics must match interpreter.
  // `sma` returns NaN before warmup; condition is directly NaN.
  jitse::Lexer nan_lexer("signal nan_cond = if sma(mid(AAPL), 3) then 1.0 else -1.0");
  jitse::Parser nan_parser(nan_lexer.Tokenize());
  jitse::SignalDef nan_def = nan_parser.ParseSignalDef();
  const bool nan_ok = jit.Compile(nan_def, symbols);
  assert(nan_ok);
  jitse::JitCompiler::JitFn nan_fn = jit.GetFunction();
  assert(nan_fn != nullptr);

  jitse::SignalContext jit_nan_ctx;
  jitse::SignalContext interp_nan_ctx;
  market.instruments[aapl].bid = 9.0;
  market.instruments[aapl].ask = 9.0;
  double got1 = nan_fn(&market, &jit_nan_ctx);
  double exp1 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got1 - exp1) < 1e-9);
  assert(std::fabs(got1 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 12.0;
  market.instruments[aapl].ask = 12.0;
  double got2 = nan_fn(&market, &jit_nan_ctx);
  double exp2 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got2 - exp2) < 1e-9);
  assert(std::fabs(got2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 15.0;
  market.instruments[aapl].ask = 15.0;
  double got3 = nan_fn(&market, &jit_nan_ctx);
  double exp3 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got3 - exp3) < 1e-9);
  assert(std::fabs(got3 - 1.0) < 1e-9);

  jitse::Lexer z_lexer("signal z = zscore(mid(AAPL), 3)");
  jitse::Parser z_parser(z_lexer.Tokenize());
  jitse::SignalDef z_def = z_parser.ParseSignalDef();
  assert(jit.Compile(z_def, symbols));
  auto z_fn = jit.GetFunction();
  assert(z_fn != nullptr);
  jitse::SignalContext z_jit_ctx;
  jitse::SignalContext z_interp_ctx;
  market.instruments[aapl].bid = 1.0;
  market.instruments[aapl].ask = 1.0;
  z_fn(&market, &z_jit_ctx);
  interp.Evaluate(z_def, market, z_interp_ctx);
  market.instruments[aapl].bid = 2.0;
  market.instruments[aapl].ask = 2.0;
  z_fn(&market, &z_jit_ctx);
  interp.Evaluate(z_def, market, z_interp_ctx);
  market.instruments[aapl].bid = 3.0;
  market.instruments[aapl].ask = 3.0;
  double z_got = z_fn(&market, &z_jit_ctx);
  double z_exp = interp.Evaluate(z_def, market, z_interp_ctx);
  assert(std::fabs(z_got - z_exp) < 1e-9);

  jitse::Lexer v_lexer("signal v = vwap(AAPL, 3)");
  jitse::Parser v_parser(v_lexer.Tokenize());
  jitse::SignalDef v_def = v_parser.ParseSignalDef();
  assert(jit.Compile(v_def, symbols));
  auto v_fn = jit.GetFunction();
  assert(v_fn != nullptr);
  jitse::SignalContext v_jit_ctx;
  jitse::SignalContext v_interp_ctx;
  market.instruments[aapl].bid = 9.0;
  market.instruments[aapl].ask = 11.0;
  market.instruments[aapl].volume = 2.0;
  v_fn(&market, &v_jit_ctx);
  interp.Evaluate(v_def, market, v_interp_ctx);
  market.instruments[aapl].bid = 19.0;
  market.instruments[aapl].ask = 21.0;
  market.instruments[aapl].volume = 1.0;
  v_fn(&market, &v_jit_ctx);
  interp.Evaluate(v_def, market, v_interp_ctx);
  market.instruments[aapl].bid = 29.0;
  market.instruments[aapl].ask = 31.0;
  market.instruments[aapl].volume = 1.0;
  double v_got = v_fn(&market, &v_jit_ctx);
  double v_exp = interp.Evaluate(v_def, market, v_interp_ctx);
  assert(std::fabs(v_got - v_exp) < 1e-9);

  jitse::Lexer lag_lexer("signal l = lag(mid(AAPL), 2)");
  jitse::Parser lag_parser(lag_lexer.Tokenize());
  jitse::SignalDef lag_def = lag_parser.ParseSignalDef();
  assert(jit.Compile(lag_def, symbols));
  auto lag_fn = jit.GetFunction();
  assert(lag_fn != nullptr);
  jitse::SignalContext lag_jit_ctx;
  jitse::SignalContext lag_interp_ctx;
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  double lag1 = lag_fn(&market, &lag_jit_ctx);
  double lag1e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::isnan(lag1) && std::isnan(lag1e));
  market.instruments[aapl].bid = 20.0;
  market.instruments[aapl].ask = 20.0;
  double lag2 = lag_fn(&market, &lag_jit_ctx);
  double lag2e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::isnan(lag2) && std::isnan(lag2e));
  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  double lag3 = lag_fn(&market, &lag_jit_ctx);
  double lag3e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::fabs(lag3 - lag3e) < 1e-9);
  assert(std::fabs(lag3 - 10.0) < 1e-9);
  market.instruments[aapl].bid = 40.0;
  market.instruments[aapl].ask = 40.0;
  double lag4 = lag_fn(&market, &lag_jit_ctx);
  double lag4e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::fabs(lag4 - lag4e) < 1e-9);
  assert(std::fabs(lag4 - 20.0) < 1e-9);

  jitse::Lexer cross_lexer("signal c = cross_above(mid(AAPL), mid(MSFT))");
  jitse::Parser cross_parser(cross_lexer.Tokenize());
  jitse::SignalDef cross_def = cross_parser.ParseSignalDef();
  assert(jit.Compile(cross_def, symbols));
  auto cross_fn = jit.GetFunction();
  assert(cross_fn != nullptr);
  jitse::SignalContext cross_jit_ctx;
  jitse::SignalContext cross_interp_ctx;
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  const std::size_t msft = symbols.LookupId("MSFT");
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c1 = cross_fn(&market, &cross_jit_ctx);
  double c1e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c1 - c1e) < 1e-9);
  assert(std::fabs(c1 - 0.0) < 1e-9);

  market.instruments[aapl].bid = 25.0;
  market.instruments[aapl].ask = 25.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c2 = cross_fn(&market, &cross_jit_ctx);
  double c2e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c2 - c2e) < 1e-9);
  assert(std::fabs(c2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c3 = cross_fn(&market, &cross_jit_ctx);
  double c3e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c3 - c3e) < 1e-9);
  assert(std::fabs(c3 - 0.0) < 1e-9);

  jitse::Lexer cross_below_lexer("signal cb = cross_below(mid(AAPL), mid(MSFT))");
  jitse::Parser cross_below_parser(cross_below_lexer.Tokenize());
  jitse::SignalDef cross_below_def = cross_below_parser.ParseSignalDef();
  assert(jit.Compile(cross_below_def, symbols));
  auto cross_below_fn = jit.GetFunction();
  assert(cross_below_fn != nullptr);
  jitse::SignalContext cb_jit_ctx;
  jitse::SignalContext cb_interp_ctx;

  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb1 = cross_below_fn(&market, &cb_jit_ctx);
  double cb1e = interp.Evaluate(cross_below_def, market, cb_interp_ctx);
  assert(std::fabs(cb1 - cb1e) < 1e-9);
  assert(std::fabs(cb1 - 0.0) < 1e-9);

  market.instruments[aapl].bid = 15.0;
  market.instruments[aapl].ask = 15.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb2 = cross_below_fn(&market, &cb_jit_ctx);
  double cb2e = interp.Evaluate(cross_below_def, market, cb_interp_ctx);
  assert(std::fabs(cb2 - cb2e) < 1e-9);
  assert(std::fabs(cb2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb3 = cross_below_fn(&market, &cb_jit_ctx);
  double cb3e = interp.Evaluate(cross_below_def, market, cb_interp_ctx);
  assert(std::fabs(cb3 - cb3e) < 1e-9);
  assert(std::fabs(cb3 - 0.0) < 1e-9);

  // Program-level parity test: compare CompileProgram (eval_all) against
  // interpreter over a deterministic 1000-tick replay.
  const std::string prog_src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(prog_src);
  std::vector<jitse::SignalDef> prog_signals = jitse::InlineSignalDependencies(parsed);

  jitse::SymbolTable prog_symbols;
  for (const auto& s : prog_signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) {
      prog_symbols.RegisterOrGetId(t);
    }
  }
  if (prog_symbols.LookupId("AAPL") != 0) {
    // no-op: ensures AAPL exists and keeps compiler from dropping lookup path
  }

  jitse::JitCompiler prog_jit;
  assert(prog_jit.IsAvailable());
  assert(prog_jit.CompileProgram(prog_signals, prog_symbols));
  auto program_fn = prog_jit.GetProgramFunction();
  assert(program_fn != nullptr);

  jitse::Interpreter prog_interp(prog_symbols);
  std::vector<jitse::SignalContext> per_signal_ctx(prog_signals.size());
  for (std::size_t i = 0; i < prog_signals.size(); ++i) {
    const std::int64_t max_id = jitse::AllocateNodeIds(prog_signals[i]);
    if (max_id > 0) {
      jitse::EnsureNodeCapacity(per_signal_ctx[i], static_cast<std::size_t>(max_id));
    }
  }

  jitse::SignalContext program_ctx;
  jitse::MarketState prog_market;
  jitse::MarketSimulator prog_sim(2026, 1);
  std::vector<double> jit_outputs(prog_signals.size(), 0.0);
  std::vector<double> interp_outputs(prog_signals.size(), 0.0);

  for (std::size_t i = 0; i < 1000; ++i) {
    const jitse::MarketEvent ev = prog_sim.NextEvent(1000);
    prog_market.instruments[ev.instrument_id].bid = ev.bid;
    prog_market.instruments[ev.instrument_id].ask = ev.ask;
    prog_market.current_time_ns = ev.timestamp_ns;

    std::fill(jit_outputs.begin(), jit_outputs.end(), 0.0);
    program_fn(&prog_market, &program_ctx, jit_outputs.data());

    for (std::size_t s = 0; s < prog_signals.size(); ++s) {
      interp_outputs[s] = prog_interp.Evaluate(prog_signals[s], prog_market, per_signal_ctx[s]);
      const double jv = jit_outputs[s];
      const double iv = interp_outputs[s];
      if (std::isnan(iv) || std::isnan(jv)) {
        assert(std::isnan(iv) && std::isnan(jv));
      } else {
        assert(std::fabs(jv - iv) < 1e-8);
      }
    }
  }

  return 0;
}
