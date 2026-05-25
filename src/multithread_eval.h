#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime.h"

namespace jitse {

struct SymbolShard {
  std::size_t begin = 0;
  std::size_t end = 0;
  [[nodiscard]] std::size_t Size() const { return end - begin; }
};

// Partition [0, n_symbols) into n_threads contiguous shards (last shard takes remainder).
SymbolShard ComputeSymbolShard(std::size_t n_symbols, std::size_t n_threads, std::size_t thread_index);

void EvaluateAllSymbolsSequential(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& arena,
    ProgramStepFn fn,
    double* outputs,
    std::size_t outputs_per_symbol);

// Evaluate symbols [shard.begin, shard.end) using thread-local arena indices [0, shard.Size()).
void EvaluateSymbolShard(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& thread_arena,
    ProgramStepFn fn,
    const SymbolShard& shard,
    double* outputs,
    std::size_t outputs_per_symbol);

std::uint64_t HashOutputBuffer(const double* data, std::size_t count);

}  // namespace jitse
