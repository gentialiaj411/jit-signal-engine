#include "multithread_eval.h"

#include <cstring>
#include <stdexcept>

namespace jitse {

SymbolShard ComputeSymbolShard(std::size_t n_symbols, std::size_t n_threads, std::size_t thread_index) {
  if (n_threads == 0) {
    throw std::runtime_error("ComputeSymbolShard: n_threads must be >= 1");
  }
  if (thread_index >= n_threads) {
    throw std::runtime_error("ComputeSymbolShard: thread_index out of range");
  }
  const std::size_t base = n_symbols / n_threads;
  const std::size_t rem = n_symbols % n_threads;
  SymbolShard shard;
  shard.begin = thread_index * base + (thread_index < rem ? thread_index : rem);
  const std::size_t extra = thread_index < rem ? 1 : 0;
  shard.end = shard.begin + base + extra;
  return shard;
}

void EvaluateAllSymbolsSequential(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& arena,
    ProgramStepFn fn,
    double* outputs,
    std::size_t outputs_per_symbol) {
  EvaluateAllSymbols(per_symbol_market, arena, fn, outputs, outputs_per_symbol);
}

void EvaluateSymbolShard(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& thread_arena,
    ProgramStepFn fn,
    const SymbolShard& shard,
    double* outputs,
    std::size_t outputs_per_symbol) {
  if (thread_arena.NumSymbols() != shard.Size()) {
    throw std::runtime_error("EvaluateSymbolShard: thread arena size mismatch");
  }
  for (std::size_t global_s = shard.begin; global_s < shard.end; ++global_s) {
    const std::uint32_t local_s = static_cast<std::uint32_t>(global_s - shard.begin);
    fn(&per_symbol_market[global_s], &thread_arena, local_s, outputs + global_s * outputs_per_symbol);
  }
}

std::uint64_t HashOutputBuffer(const double* data, std::size_t count) {
  std::uint64_t h = 14695981039346656037ull;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  const std::size_t nbytes = count * sizeof(double);
  for (std::size_t i = 0; i < nbytes; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace jitse
