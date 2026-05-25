#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "jit_compiler.h"
#include "market_sim.h"
#include "multithread_eval.h"
#include "runtime.h"
#include "signal_program.h"

int main() {
  constexpr std::size_t kSymbols = 256;
  constexpr std::size_t kTicks = 400;
  constexpr std::size_t kThreads = 8;

  const std::string src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  jitse::AllocateProgramNodeIds(signals);

  jitse::SymbolTable symbols;
  symbols.RegisterOrGetId("AAPL");
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  jitse::JitCompiler jit;
  if (!jit.IsAvailable()) {
    std::cout << "multithread_equivalence_test=skip (no LLVM)\n";
    return 0;
  }
  if (!jit.CompileProgram(signals, symbols) || jit.GetProgramFunction() == nullptr) {
    std::cerr << "CompileProgram failed\n";
    return 1;
  }
  const auto fn = jit.GetProgramFunction();
  const std::size_t osp = signals.size();

  std::vector<jitse::MarketState> markets(kSymbols);
  std::vector<jitse::MarketSimulator> sims;
  sims.reserve(kSymbols);
  for (std::size_t s = 0; s < kSymbols; ++s) {
    sims.emplace_back(static_cast<std::uint64_t>(5000 + s), 1);
  }

  std::vector<double> st_outputs(kSymbols * osp, 0.0);
  std::vector<double> mt_outputs(kSymbols * osp, 0.0);

  jitse::MultiSymbolSignalContext st_arena(kSymbols);
  for (std::size_t s = 0; s < kSymbols; ++s) {
    for (const auto& sig : signals) {
      jitse::PrewarmSignalContext(st_arena, static_cast<std::uint32_t>(s), sig);
    }
  }

  struct ThreadCtx {
    jitse::SymbolShard shard;
    jitse::MultiSymbolSignalContext arena;
    explicit ThreadCtx(std::size_t n) : arena(n) {}
  };
  std::vector<ThreadCtx> ctxs;
  ctxs.reserve(kThreads);
  for (std::size_t t = 0; t < kThreads; ++t) {
    auto shard = jitse::ComputeSymbolShard(kSymbols, kThreads, t);
    ctxs.emplace_back(shard.Size());
    ctxs.back().shard = shard;
    for (std::size_t local_s = 0; local_s < shard.Size(); ++local_s) {
      for (const auto& sig : signals) {
        jitse::PrewarmSignalContext(ctxs.back().arena, static_cast<std::uint32_t>(local_s), sig);
      }
    }
  }

  for (std::size_t tick = 0; tick < kTicks; ++tick) {
  for (std::size_t s = 0; s < kSymbols; ++s) {
    const auto ev = sims[s].NextEvent(1000);
    markets[s].instruments[0].bid = ev.bid;
    markets[s].instruments[0].ask = ev.ask;
    markets[s].current_time_ns = ev.timestamp_ns;
  }

  jitse::EvaluateAllSymbolsSequential(markets, st_arena, fn, st_outputs.data(), osp);

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      const auto& shard = ctxs[t].shard;
      for (std::size_t global_s = shard.begin; global_s < shard.end; ++global_s) {
        const std::uint32_t local_s = static_cast<std::uint32_t>(global_s - shard.begin);
        fn(&markets[global_s], &ctxs[t].arena, local_s, mt_outputs.data() + global_s * osp);
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }
  }

  const std::uint64_t st_hash = jitse::HashOutputBuffer(st_outputs.data(), st_outputs.size());
  const std::uint64_t mt_hash = jitse::HashOutputBuffer(mt_outputs.data(), mt_outputs.size());
  if (st_hash != mt_hash) {
    std::cerr << "hash mismatch st=" << st_hash << " mt=" << mt_hash << "\n";
    for (std::size_t i = 0; i < st_outputs.size(); ++i) {
      if (st_outputs[i] != mt_outputs[i]) {
        std::cerr << "first mismatch index=" << i << " st=" << st_outputs[i] << " mt=" << mt_outputs[i] << "\n";
        return 1;
      }
    }
    return 1;
  }

  std::cout << "multithread_equivalence_test=pass\n";
  std::cout << "symbols=" << kSymbols << " ticks=" << kTicks << " threads=" << kThreads << "\n";
  std::cout << "output_hash=" << st_hash << "\n";
  return 0;
}
