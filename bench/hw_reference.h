#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime.h"

namespace jitse {
namespace hw {

enum class Profile {
  kNone,
  kSpread,
  kMomentum,
  kSpreadZ,
  kZ,
  kDev,
  kFilteredMomentum,
};

Profile ProfileForBenchmark(bool all_signals_mode, const std::string& signal_name);

struct EmaSlot {
  double value = 0.0;
  double alpha = 0.0;
  bool initialized = false;
};

struct State {
  EmaSlot ema_short{};
  EmaSlot ema_long{};
  EmaSlot ema_aapl{};
  EmaSlot ema_msft{};
  EmaSlot ema_fast{};
  EmaSlot ema_slow{};
  RingStatsState rstd_aapl{};
  RingStatsState rstd_aapl_dev{};
  RingStatsState zscore_ring{};
  VwapState vwap_aapl{};
};

void ResetState(State& st, Profile profile);
double Tick(State& st, Profile profile, const MarketState& market);

}  // namespace hw
}  // namespace jitse
