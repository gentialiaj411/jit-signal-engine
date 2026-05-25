#include <cassert>
#include <cmath>
#include <cstdlib>
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
  auto Require = [](bool cond, const char* msg) {
    if (!cond) {
      std::cerr << msg << "\n";
      return false;
    }
    return true;
  };

  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) {
    // Acceptable in environments without LLVM installed.
    std::cout << "jit_available=false\n";
    return 0;
  }
  std::cout << "jit_available=true\n";
  std::cout << "jit_has_avx2=" << (jit.HasAVX2() ? "true" : "false") << "\n";

#ifdef _WIN32
  _putenv_s("JITSE_FORCE_DISABLE_AVX2", "1");
#else
  setenv("JITSE_FORCE_DISABLE_AVX2", "1", 1);
#endif
  jitse::JitCompiler jit_forced_scalar;
  assert(!jit_forced_scalar.HasAVX2());
#ifdef _WIN32
  _putenv_s("JITSE_FORCE_DISABLE_AVX2", "");
#else
  unsetenv("JITSE_FORCE_DISABLE_AVX2");
#endif

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");
  symbols.RegisterOrGetId("MSFT");

  jitse::Lexer lexer("signal x = mid(AAPL) + 1");
  jitse::Parser parser(lexer.Tokenize());
  jitse::SignalDef def = parser.ParseSignalDef();
  jitse::AllocateNodeIds(def);

  const bool ok = jit.Compile(def, symbols);
  if (!Require(ok, "Compile failed for signal x")) return 1;
  jitse::JitCompiler::JitFn fn = jit.GetFunction();
  if (!Require(fn != nullptr, "GetFunction returned null for signal x")) return 1;
  std::cout << "jit_mode=enabled\n";

  jitse::MarketState market;
  const std::size_t aapl = symbols.LookupId("AAPL");
  market.instruments[aapl].bid = 100.0;
  market.instruments[aapl].ask = 102.0;
  jitse::MultiSymbolSignalContext ctx(1);
  jitse::PrewarmSignalContext(ctx, 0, def);
  const double out = fn(&market, &ctx, 0);

  jitse::Interpreter interp(symbols);
  jitse::SignalContext interp_ctx;
  jitse::PrewarmSignalContext(interp_ctx, def);
  const double expected = interp.Evaluate(def, market, interp_ctx);
  assert(std::fabs(out - expected) < 1e-9);

  auto compile_and_compare = [&](const char* source, const char* label, double bid, double ask) {
    jitse::Lexer local_lexer(source);
    jitse::Parser local_parser(local_lexer.Tokenize());
    jitse::SignalDef local_def = local_parser.ParseSignalDef();
    jitse::AllocateNodeIds(local_def);
    if (!Require(jit.Compile(local_def, symbols), label)) return false;
    auto local_fn = jit.GetFunction();
    if (!Require(local_fn != nullptr, "GetFunction returned null for math builtin parity")) return false;

    jitse::MarketState local_market;
    local_market.instruments[aapl].bid = bid;
    local_market.instruments[aapl].ask = ask;
    jitse::MultiSymbolSignalContext local_jit_ctx(1);
    jitse::SignalContext local_interp_ctx;
    jitse::PrewarmSignalContext(local_jit_ctx, 0, local_def);
    jitse::PrewarmSignalContext(local_interp_ctx, local_def);

    const double got = local_fn(&local_market, &local_jit_ctx, 0);
    const double exp = interp.Evaluate(local_def, local_market, local_interp_ctx);
    if (std::isnan(exp)) return std::isnan(got);
    return std::fabs(got - exp) < 1e-9;
  };

  if (!Require(compile_and_compare("signal a = abs(mid(AAPL) - 105.0)", "Compile failed for abs parity", 99.0, 101.0),
               "abs JIT/interpreter parity failed")) {
    return 1;
  }
  if (!Require(compile_and_compare("signal s = sqrt(mid(AAPL))", "Compile failed for sqrt parity", 24.0, 26.0),
               "sqrt JIT/interpreter parity failed")) {
    return 1;
  }
  if (!Require(compile_and_compare("signal l = log(mid(AAPL))", "Compile failed for log parity", 9.0, 11.0),
               "log JIT/interpreter parity failed")) {
    return 1;
  }
  if (!Require(compile_and_compare("signal m = abs(log(sqrt(mid(AAPL))) - log(5.0))",
                                   "Compile failed for nested math builtin parity",
                                   24.0,
                                   26.0),
               "nested math builtin JIT/interpreter parity failed")) {
    return 1;
  }

  // Regression test: NaN condition semantics must match interpreter.
  // `sma` returns NaN before warmup; condition is directly NaN.
  jitse::Lexer nan_lexer("signal nan_cond = if sma(mid(AAPL), 3) then 1.0 else -1.0");
  jitse::Parser nan_parser(nan_lexer.Tokenize());
  jitse::SignalDef nan_def = nan_parser.ParseSignalDef();
  jitse::AllocateNodeIds(nan_def);
  const bool nan_ok = jit.Compile(nan_def, symbols);
  if (!Require(nan_ok, "Compile failed for signal nan_cond")) return 1;
  jitse::JitCompiler::JitFn nan_fn = jit.GetFunction();
  if (!Require(nan_fn != nullptr, "GetFunction returned null for signal nan_cond")) return 1;

  jitse::MultiSymbolSignalContext jit_nan_ctx(1);
  jitse::SignalContext interp_nan_ctx;
  jitse::PrewarmSignalContext(jit_nan_ctx, 0, nan_def);
  jitse::PrewarmSignalContext(interp_nan_ctx, nan_def);
  market.instruments[aapl].bid = 9.0;
  market.instruments[aapl].ask = 9.0;
  double got1 = nan_fn(&market, &jit_nan_ctx, 0);
  double exp1 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got1 - exp1) < 1e-9);
  assert(std::fabs(got1 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 12.0;
  market.instruments[aapl].ask = 12.0;
  double got2 = nan_fn(&market, &jit_nan_ctx, 0);
  double exp2 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got2 - exp2) < 1e-9);
  assert(std::fabs(got2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 15.0;
  market.instruments[aapl].ask = 15.0;
  double got3 = nan_fn(&market, &jit_nan_ctx, 0);
  double exp3 = interp.Evaluate(nan_def, market, interp_nan_ctx);
  assert(std::fabs(got3 - exp3) < 1e-9);
  assert(std::fabs(got3 - 1.0) < 1e-9);

  jitse::Lexer z_lexer("signal z = zscore(mid(AAPL), 3)");
  jitse::Parser z_parser(z_lexer.Tokenize());
  jitse::SignalDef z_def = z_parser.ParseSignalDef();
  jitse::AllocateNodeIds(z_def);
  if (!Require(jit.Compile(z_def, symbols), "Compile failed for signal z")) return 1;
  auto z_fn = jit.GetFunction();
  if (!Require(z_fn != nullptr, "GetFunction returned null for signal z")) return 1;
  jitse::MultiSymbolSignalContext z_jit_ctx(1);
  jitse::SignalContext z_interp_ctx;
  jitse::PrewarmSignalContext(z_jit_ctx, 0, z_def);
  jitse::PrewarmSignalContext(z_interp_ctx, z_def);
  market.instruments[aapl].bid = 1.0;
  market.instruments[aapl].ask = 1.0;
  z_fn(&market, &z_jit_ctx, 0);
  interp.Evaluate(z_def, market, z_interp_ctx);
  market.instruments[aapl].bid = 2.0;
  market.instruments[aapl].ask = 2.0;
  z_fn(&market, &z_jit_ctx, 0);
  interp.Evaluate(z_def, market, z_interp_ctx);
  market.instruments[aapl].bid = 3.0;
  market.instruments[aapl].ask = 3.0;
  double z_got = z_fn(&market, &z_jit_ctx, 0);
  double z_exp = interp.Evaluate(z_def, market, z_interp_ctx);
  assert(std::fabs(z_got - z_exp) < 1e-9);

  jitse::Lexer v_lexer("signal v = vwap(AAPL, 3)");
  jitse::Parser v_parser(v_lexer.Tokenize());
  jitse::SignalDef v_def = v_parser.ParseSignalDef();
  jitse::AllocateNodeIds(v_def);
  if (!Require(jit.Compile(v_def, symbols), "Compile failed for signal v")) return 1;
  auto v_fn = jit.GetFunction();
  if (!Require(v_fn != nullptr, "GetFunction returned null for signal v")) return 1;
  jitse::MultiSymbolSignalContext v_jit_ctx(1);
  jitse::SignalContext v_interp_ctx;
  jitse::PrewarmSignalContext(v_jit_ctx, 0, v_def);
  jitse::PrewarmSignalContext(v_interp_ctx, v_def);
  market.instruments[aapl].bid = 9.0;
  market.instruments[aapl].ask = 11.0;
  market.instruments[aapl].volume = 2.0;
  v_fn(&market, &v_jit_ctx, 0);
  interp.Evaluate(v_def, market, v_interp_ctx);
  market.instruments[aapl].bid = 19.0;
  market.instruments[aapl].ask = 21.0;
  market.instruments[aapl].volume = 1.0;
  v_fn(&market, &v_jit_ctx, 0);
  interp.Evaluate(v_def, market, v_interp_ctx);
  market.instruments[aapl].bid = 29.0;
  market.instruments[aapl].ask = 31.0;
  market.instruments[aapl].volume = 1.0;
  double v_got = v_fn(&market, &v_jit_ctx, 0);
  double v_exp = interp.Evaluate(v_def, market, v_interp_ctx);
  assert(std::fabs(v_got - v_exp) < 1e-9);

  jitse::Lexer lag_lexer("signal l = lag(mid(AAPL), 2)");
  jitse::Parser lag_parser(lag_lexer.Tokenize());
  jitse::SignalDef lag_def = lag_parser.ParseSignalDef();
  jitse::AllocateNodeIds(lag_def);
  if (!Require(jit.Compile(lag_def, symbols), "Compile failed for signal l")) return 1;
  auto lag_fn = jit.GetFunction();
  if (!Require(lag_fn != nullptr, "GetFunction returned null for signal l")) return 1;
  jitse::MultiSymbolSignalContext lag_jit_ctx(1);
  jitse::SignalContext lag_interp_ctx;
  jitse::PrewarmSignalContext(lag_jit_ctx, 0, lag_def);
  jitse::PrewarmSignalContext(lag_interp_ctx, lag_def);
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  double lag1 = lag_fn(&market, &lag_jit_ctx, 0);
  double lag1e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::isnan(lag1) && std::isnan(lag1e));
  market.instruments[aapl].bid = 20.0;
  market.instruments[aapl].ask = 20.0;
  double lag2 = lag_fn(&market, &lag_jit_ctx, 0);
  double lag2e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::isnan(lag2) && std::isnan(lag2e));
  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  double lag3 = lag_fn(&market, &lag_jit_ctx, 0);
  double lag3e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::fabs(lag3 - lag3e) < 1e-9);
  assert(std::fabs(lag3 - 10.0) < 1e-9);
  market.instruments[aapl].bid = 40.0;
  market.instruments[aapl].ask = 40.0;
  double lag4 = lag_fn(&market, &lag_jit_ctx, 0);
  double lag4e = interp.Evaluate(lag_def, market, lag_interp_ctx);
  assert(std::fabs(lag4 - lag4e) < 1e-9);
  assert(std::fabs(lag4 - 20.0) < 1e-9);

  jitse::Lexer rmin_lexer("signal rmin = rolling_min(mid(AAPL), 3)");
  jitse::Parser rmin_parser(rmin_lexer.Tokenize());
  jitse::SignalDef rmin_def = rmin_parser.ParseSignalDef();
  jitse::AllocateNodeIds(rmin_def);
  if (!Require(jit.Compile(rmin_def, symbols), "Compile failed for signal rmin")) return 1;
  auto rmin_fn = jit.GetFunction();
  if (!Require(rmin_fn != nullptr, "GetFunction returned null for signal rmin")) return 1;
  jitse::MultiSymbolSignalContext rmin_jit_ctx(1);
  jitse::SignalContext rmin_interp_ctx;
  jitse::PrewarmSignalContext(rmin_jit_ctx, 0, rmin_def);
  jitse::PrewarmSignalContext(rmin_interp_ctx, rmin_def);
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  double rmin1 = rmin_fn(&market, &rmin_jit_ctx, 0);
  double rmin1e = interp.Evaluate(rmin_def, market, rmin_interp_ctx);
  assert(std::isnan(rmin1) && std::isnan(rmin1e));
  market.instruments[aapl].bid = 20.0;
  market.instruments[aapl].ask = 20.0;
  double rmin2 = rmin_fn(&market, &rmin_jit_ctx, 0);
  double rmin2e = interp.Evaluate(rmin_def, market, rmin_interp_ctx);
  assert(std::isnan(rmin2) && std::isnan(rmin2e));
  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  double rmin3 = rmin_fn(&market, &rmin_jit_ctx, 0);
  double rmin3e = interp.Evaluate(rmin_def, market, rmin_interp_ctx);
  assert(std::fabs(rmin3 - rmin3e) < 1e-9);
  market.instruments[aapl].bid = 40.0;
  market.instruments[aapl].ask = 40.0;
  double rmin4 = rmin_fn(&market, &rmin_jit_ctx, 0);
  double rmin4e = interp.Evaluate(rmin_def, market, rmin_interp_ctx);
  assert(std::fabs(rmin4 - rmin4e) < 1e-9);
  market.instruments[aapl].bid = 50.0;
  market.instruments[aapl].ask = 50.0;
  double rmin5 = rmin_fn(&market, &rmin_jit_ctx, 0);
  double rmin5e = interp.Evaluate(rmin_def, market, rmin_interp_ctx);
  assert(std::fabs(rmin5 - rmin5e) < 1e-9);

  jitse::Lexer rmax_lexer("signal rmax = rolling_max(mid(AAPL), 3)");
  jitse::Parser rmax_parser(rmax_lexer.Tokenize());
  jitse::SignalDef rmax_def = rmax_parser.ParseSignalDef();
  jitse::AllocateNodeIds(rmax_def);
  if (!Require(jit.Compile(rmax_def, symbols), "Compile failed for signal rmax")) return 1;
  auto rmax_fn = jit.GetFunction();
  if (!Require(rmax_fn != nullptr, "GetFunction returned null for signal rmax")) return 1;
  jitse::MultiSymbolSignalContext rmax_jit_ctx(1);
  jitse::SignalContext rmax_interp_ctx;
  jitse::PrewarmSignalContext(rmax_jit_ctx, 0, rmax_def);
  jitse::PrewarmSignalContext(rmax_interp_ctx, rmax_def);
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  double rmax1 = rmax_fn(&market, &rmax_jit_ctx, 0);
  double rmax1e = interp.Evaluate(rmax_def, market, rmax_interp_ctx);
  assert(std::isnan(rmax1) && std::isnan(rmax1e));
  market.instruments[aapl].bid = 20.0;
  market.instruments[aapl].ask = 20.0;
  double rmax2 = rmax_fn(&market, &rmax_jit_ctx, 0);
  double rmax2e = interp.Evaluate(rmax_def, market, rmax_interp_ctx);
  assert(std::isnan(rmax2) && std::isnan(rmax2e));
  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  double rmax3 = rmax_fn(&market, &rmax_jit_ctx, 0);
  double rmax3e = interp.Evaluate(rmax_def, market, rmax_interp_ctx);
  assert(std::fabs(rmax3 - rmax3e) < 1e-9);
  market.instruments[aapl].bid = 40.0;
  market.instruments[aapl].ask = 40.0;
  double rmax4 = rmax_fn(&market, &rmax_jit_ctx, 0);
  double rmax4e = interp.Evaluate(rmax_def, market, rmax_interp_ctx);
  assert(std::fabs(rmax4 - rmax4e) < 1e-9);
  market.instruments[aapl].bid = 50.0;
  market.instruments[aapl].ask = 50.0;
  double rmax5 = rmax_fn(&market, &rmax_jit_ctx, 0);
  double rmax5e = interp.Evaluate(rmax_def, market, rmax_interp_ctx);
  assert(std::fabs(rmax5 - rmax5e) < 1e-9);

  jitse::Lexer cross_lexer("signal c = cross_above(mid(AAPL), mid(MSFT))");
  jitse::Parser cross_parser(cross_lexer.Tokenize());
  jitse::SignalDef cross_def = cross_parser.ParseSignalDef();
  jitse::AllocateNodeIds(cross_def);
  if (!Require(jit.Compile(cross_def, symbols), "Compile failed for signal c")) return 1;
  auto cross_fn = jit.GetFunction();
  if (!Require(cross_fn != nullptr, "GetFunction returned null for signal c")) return 1;
  jitse::MultiSymbolSignalContext cross_jit_ctx(1);
  jitse::SignalContext cross_interp_ctx;
  jitse::PrewarmSignalContext(cross_jit_ctx, 0, cross_def);
  jitse::PrewarmSignalContext(cross_interp_ctx, cross_def);
  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  const std::size_t msft = symbols.LookupId("MSFT");
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c1 = cross_fn(&market, &cross_jit_ctx, 0);
  double c1e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c1 - c1e) < 1e-9);
  assert(std::fabs(c1 - 0.0) < 1e-9);

  market.instruments[aapl].bid = 25.0;
  market.instruments[aapl].ask = 25.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c2 = cross_fn(&market, &cross_jit_ctx, 0);
  double c2e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c2 - c2e) < 1e-9);
  assert(std::fabs(c2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double c3 = cross_fn(&market, &cross_jit_ctx, 0);
  double c3e = interp.Evaluate(cross_def, market, cross_interp_ctx);
  assert(std::fabs(c3 - c3e) < 1e-9);
  assert(std::fabs(c3 - 0.0) < 1e-9);

  jitse::Lexer cross_below_lexer("signal cb = cross_below(mid(AAPL), mid(MSFT))");
  jitse::Parser cross_below_parser(cross_below_lexer.Tokenize());
  jitse::SignalDef cross_below_def = cross_below_parser.ParseSignalDef();
  jitse::AllocateNodeIds(cross_below_def);
  if (!Require(jit.Compile(cross_below_def, symbols), "Compile failed for signal cb")) return 1;
  auto cross_below_fn = jit.GetFunction();
  if (!Require(cross_below_fn != nullptr, "GetFunction returned null for signal cb")) return 1;
  jitse::MultiSymbolSignalContext cb_jit_ctx(1);
  jitse::SignalContext cb_interp_ctx;
  jitse::PrewarmSignalContext(cb_jit_ctx, 0, cross_below_def);
  jitse::PrewarmSignalContext(cb_interp_ctx, cross_below_def);

  market.instruments[aapl].bid = 30.0;
  market.instruments[aapl].ask = 30.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb1 = cross_below_fn(&market, &cb_jit_ctx, 0);
  double cb1e = interp.Evaluate(cross_below_def, market, cb_interp_ctx);
  assert(std::fabs(cb1 - cb1e) < 1e-9);
  assert(std::fabs(cb1 - 0.0) < 1e-9);

  market.instruments[aapl].bid = 15.0;
  market.instruments[aapl].ask = 15.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb2 = cross_below_fn(&market, &cb_jit_ctx, 0);
  double cb2e = interp.Evaluate(cross_below_def, market, cb_interp_ctx);
  assert(std::fabs(cb2 - cb2e) < 1e-9);
  assert(std::fabs(cb2 - 1.0) < 1e-9);

  market.instruments[aapl].bid = 10.0;
  market.instruments[aapl].ask = 10.0;
  market.instruments[msft].bid = 20.0;
  market.instruments[msft].ask = 20.0;
  double cb3 = cross_below_fn(&market, &cb_jit_ctx, 0);
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
  std::vector<jitse::SignalDef> prog_signals = jitse::ParseSignalProgram(prog_src);

  jitse::SymbolTable prog_symbols;
  for (const auto& s : prog_signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) {
      prog_symbols.RegisterOrGetId(t);
    }
  }
  if (prog_symbols.LookupId("AAPL") != 0) {
    // no-op: ensures AAPL exists and keeps compiler from dropping lookup path
  }

  jitse::AllocateProgramNodeIds(prog_signals);

  jitse::JitCompiler prog_jit;
  if (!Require(prog_jit.IsAvailable(), "Program JIT is unavailable")) return 1;
  if (!Require(prog_jit.CompileProgram(prog_signals, prog_symbols), "CompileProgram failed")) return 1;
  auto program_fn = prog_jit.GetProgramFunction();
  if (!Require(program_fn != nullptr, "GetProgramFunction returned null")) return 1;

  jitse::MultiSymbolSignalContext program_ctx(1);
  for (std::size_t i = 0; i < prog_signals.size(); ++i) {
    jitse::PrewarmSignalContext(program_ctx, 0, prog_signals[i]);
  }

  jitse::MarketState prog_market;
  jitse::MarketSimulator prog_sim(2026, 1);
  std::vector<double> jit_outputs(prog_signals.size(), 0.0);

  for (std::size_t i = 0; i < 1000; ++i) {
    const jitse::MarketEvent ev = prog_sim.NextEvent(1000);
    prog_market.instruments[ev.instrument_id].bid = ev.bid;
    prog_market.instruments[ev.instrument_id].ask = ev.ask;
    prog_market.current_time_ns = ev.timestamp_ns;

    std::fill(jit_outputs.begin(), jit_outputs.end(), 0.0);
    program_fn(&prog_market, &program_ctx, 0, jit_outputs.data());

    const double short_ma_v = jit_outputs[0];
    const double long_ma_v = jit_outputs[1];
    const double vol_v = jit_outputs[2];
    const double raw_v = jit_outputs[3];
    if (!Require(std::fabs(raw_v - (short_ma_v - long_ma_v)) < 1e-9, "Program raw output did not reuse prior signal outputs")) return 1;
    if (short_ma_v > long_ma_v && vol_v > 0.0) {
      if (!Require(std::isfinite(jit_outputs[4]), "Program filtered output was non-finite with positive finite denominator")) return 1;
      if (!Require(std::fabs(jit_outputs[4] - (raw_v / vol_v)) < 1e-8, "Program filtered output did not reuse raw/vol outputs")) return 1;
    } else {
      if (!Require(std::fabs(jit_outputs[4]) < 1e-9, "Program filtered output should be zero when condition is false")) return 1;
    }
  }

  return 0;
}

