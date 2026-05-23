#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "market_sim.h"
#include "runtime.h"
#include "signal_program.h"

namespace {

std::atomic<std::uint64_t> g_allocations{0};
thread_local bool g_count_allocations = false;

struct AllocationScope {
  explicit AllocationScope(bool enabled) : prev(g_count_allocations) { g_count_allocations = enabled; }
  ~AllocationScope() { g_count_allocations = prev; }
  bool prev;
};

void ResetAllocations() { g_allocations.store(0, std::memory_order_relaxed); }
std::uint64_t AllocationCount() { return g_allocations.load(std::memory_order_relaxed); }

}  // namespace

void* operator new(std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }

int main() {
  const std::string src =
      "signal short_ma = ema(mid(AAPL), 10)\n"
      "signal long_ma = ema(mid(AAPL), 60)\n"
      "signal vol = rolling_std(mid(AAPL), 30)\n"
      "signal raw = short_ma - long_ma\n"
      "signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0\n";

  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  std::vector<jitse::SignalDef> signals = jitse::InlineSignalDependencies(parsed);
  for (auto& s : signals) jitse::AllocateNodeIds(s);

  jitse::SymbolTable symbols;
  for (const auto& s : signals) {
    for (const auto& t : jitse::CollectTickerSymbols(s)) symbols.RegisterOrGetId(t);
  }
  for (auto& s : signals) jitse::BindSymbolIds(s, symbols);

  const std::size_t events = 20000;
  const std::size_t instrument_count = 1;
  constexpr std::size_t warmup = 5000;

  jitse::MarketState market;
  jitse::SignalContext interp_ctx;
  for (const auto& s : signals) jitse::PrewarmSignalContext(interp_ctx, s);
  jitse::Interpreter interp(symbols);
  jitse::MarketSimulator sim(42, instrument_count);

  for (std::size_t i = 0; i < warmup; ++i) {
    const auto ev = sim.NextEvent(1000);
    market.instruments[ev.instrument_id].bid = ev.bid;
    market.instruments[ev.instrument_id].ask = ev.ask;
    market.current_time_ns = ev.timestamp_ns;
    for (const auto& s : signals) (void)interp.Evaluate(s, market, interp_ctx);
  }

  ResetAllocations();
  volatile double interp_sink = 0.0;
  {
    AllocationScope scope(true);
    for (std::size_t i = 0; i < events; ++i) {
      const auto ev = sim.NextEvent(1000);
      market.instruments[ev.instrument_id].bid = ev.bid;
      market.instruments[ev.instrument_id].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      for (const auto& s : signals) interp_sink += interp.Evaluate(s, market, interp_ctx);
    }
  }
  const std::uint64_t interp_allocs = AllocationCount();
  assert(interp_allocs == 0 && "Interpreter hot path must not allocate after warmup");

  jitse::JitCompiler jit;
  if (jit.IsAvailable() && jit.CompileProgram(signals, symbols) && jit.GetProgramFunction() != nullptr) {
    jitse::MultiSymbolSignalContext jit_ctx(1);
    for (const auto& s : signals) jitse::PrewarmSignalContext(jit_ctx, 0, s);
    std::vector<double> outputs(signals.size(), 0.0);
    jitse::MarketSimulator jsim(42, instrument_count);
    for (std::size_t i = 0; i < warmup; ++i) {
      const auto ev = jsim.NextEvent(1000);
      market.instruments[ev.instrument_id].bid = ev.bid;
      market.instruments[ev.instrument_id].ask = ev.ask;
      market.current_time_ns = ev.timestamp_ns;
      jit.GetProgramFunction()(&market, &jit_ctx, 0, outputs.data());
    }
    ResetAllocations();
    volatile double jit_sink = 0.0;
    {
      AllocationScope scope(true);
      for (std::size_t i = 0; i < events; ++i) {
        const auto ev = jsim.NextEvent(1000);
        market.instruments[ev.instrument_id].bid = ev.bid;
        market.instruments[ev.instrument_id].ask = ev.ask;
        market.current_time_ns = ev.timestamp_ns;
        jit.GetProgramFunction()(&market, &jit_ctx, 0, outputs.data());
        jit_sink += outputs.back();
      }
    }
    const std::uint64_t jit_allocs = AllocationCount();
    assert(jit_allocs == 0 && "JIT hot path must not allocate after warmup");
    (void)jit_sink;
  }

  (void)interp_sink;
  return 0;
}
