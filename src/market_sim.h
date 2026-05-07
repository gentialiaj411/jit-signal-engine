#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "runtime.h"

namespace jitse {

struct MarketEvent {
  std::size_t instrument_id = 0;
  double bid = 0.0;
  double ask = 0.0;
  std::uint64_t timestamp_ns = 0;
};

class MarketSimulator {
 public:
  MarketSimulator(std::uint64_t seed, std::size_t instrument_count);
  MarketEvent NextEvent(std::uint64_t dt_ns);

 private:
  std::mt19937_64 rng_;
  std::normal_distribution<double> normal_{0.0, 1.0};
  std::uniform_int_distribution<std::size_t> instrument_dist_;
  std::vector<double> mids_;
  std::uint64_t ts_ns_ = 0;
};

}  // namespace jitse

