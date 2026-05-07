#include "market_sim.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jitse {

MarketSimulator::MarketSimulator(std::uint64_t seed, std::size_t instrument_count)
    : rng_(seed), instrument_dist_(0, instrument_count == 0 ? 0 : instrument_count - 1), mids_(instrument_count, 100.0) {
  if (instrument_count == 0 || instrument_count > kMaxInstruments) {
    throw std::runtime_error("MarketSimulator instrument_count must be in [1, kMaxInstruments]");
  }
}

MarketEvent MarketSimulator::NextEvent(std::uint64_t dt_ns) {
  ts_ns_ += dt_ns;
  const std::size_t id = instrument_dist_(rng_);

  // Geometric Brownian motion style update for positive prices.
  const double sigma = 0.0005;
  const double shock = sigma * normal_(rng_);
  mids_[id] = std::max(0.01, mids_[id] * std::exp(shock));

  const double half_spread = 0.005;
  return MarketEvent{
      id,
      mids_[id] - half_spread,
      mids_[id] + half_spread,
      ts_ns_,
  };
}

}  // namespace jitse

