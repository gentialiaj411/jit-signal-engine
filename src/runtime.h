#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace jitse {

constexpr std::size_t kMaxInstruments = 1024;
struct SignalDef;

struct alignas(64) InstrumentState {
  double bid = 0.0;
  double ask = 0.0;
  double last_price = 0.0;
  double volume = 0.0;
  std::uint64_t last_update_ns = 0;
};

struct MarketState {
  std::array<InstrumentState, kMaxInstruments> instruments{};
  std::uint64_t current_time_ns = 0;
};

struct RingStatsState {
  std::vector<double> buffer;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double sum = 0.0L;
  long double sumsq = 0.0L;
};

struct EMAState {
  double value = 0.0;
  bool initialized = false;
};

struct VwapState {
  std::vector<double> price_buf;
  std::vector<double> vol_buf;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double sum_pv = 0.0L;
  long double sum_vol = 0.0L;
};

struct MonoDequeState {
  std::vector<std::pair<std::size_t, double>> buf;
  std::size_t head = 0;
  std::size_t count = 0;
  std::size_t cap = 0;
};

struct LagState {
  std::vector<double> buffer;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  std::size_t total_samples = 0;
};

struct CrossState {
  double prev_a = 0.0;
  double prev_b = 0.0;
  bool initialized = false;
};

struct SignalContext {
  std::vector<EMAState> ema_states;
  std::vector<RingStatsState> sma_states;
  std::vector<RingStatsState> rolling_std_states;
  std::vector<RingStatsState> zscore_states;
  std::vector<VwapState> vwap_states;
  std::vector<LagState> lag_states;
  std::vector<CrossState> cross_states;
  std::vector<MonoDequeState> rolling_min_deques;
  std::vector<MonoDequeState> rolling_max_deques;
  std::vector<std::size_t> rolling_min_indices;
  std::vector<std::size_t> rolling_max_indices;
};

class SymbolTable {
 public:
  std::size_t RegisterOrGetId(const std::string& symbol);
  std::size_t LookupId(const std::string& symbol) const;

 private:
  std::unordered_map<std::string, std::size_t> symbol_to_id_;
  std::size_t next_id_ = 0;
};

void RingStatsPush(RingStatsState& state, std::size_t period, double sample);
double RingStatsMean(const RingStatsState& state);
double RingStatsStddevSample(const RingStatsState& state);
bool RingStatsFull(const RingStatsState& state);
void VwapPush(VwapState& state, std::size_t period, double price, double volume);
double VwapValue(const VwapState& state);
bool VwapFull(const VwapState& state);
void LagPush(LagState& state, std::size_t period, double sample);
double LagValue(const LagState& state);
void EnsureNodeCapacity(SignalContext& ctx, std::size_t node_id);
void PrewarmSignalContext(SignalContext& ctx, const SignalDef& signal);
double UpdateRollingMin(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample);
double UpdateRollingMax(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample);

extern "C" {
double jit_rt_mid(const MarketState* state, std::int64_t symbol_id);
double jit_rt_bid(const MarketState* state, std::int64_t symbol_id);
double jit_rt_ask(const MarketState* state, std::int64_t symbol_id);
double jit_rt_spread(const MarketState* state, std::int64_t symbol_id);

double jit_rt_ema(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_sma(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_rolling_std(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_zscore(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_rolling_min(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_rolling_max(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_vwap(const MarketState* state, SignalContext* ctx, std::int64_t node_id, std::int64_t symbol_id, std::int64_t period);
double jit_rt_lag(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_cross_above(SignalContext* ctx, std::int64_t node_id, double a, double b);
double jit_rt_cross_below(SignalContext* ctx, std::int64_t node_id, double a, double b);
}

}  // namespace jitse
