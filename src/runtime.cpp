#include "runtime.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>

#include "ast.h"

namespace jitse {

// Lock the lowered-state layouts the JIT codegen depends on. If any of these
// fire, jit_compiler.cpp's hardcoded field offsets are stale.
static_assert(sizeof(SmaStateLowered) == 40, "SmaStateLowered must be 40 bytes");
static_assert(offsetof(SmaStateLowered, buffer) == 0, "SmaStateLowered.buffer offset");
static_assert(offsetof(SmaStateLowered, sum) == 8, "SmaStateLowered.sum offset");
static_assert(offsetof(SmaStateLowered, head) == 16, "SmaStateLowered.head offset");
static_assert(offsetof(SmaStateLowered, count) == 24, "SmaStateLowered.count offset");
static_assert(offsetof(SmaStateLowered, capacity) == 32, "SmaStateLowered.capacity offset");

static_assert(sizeof(EmaStateLowered) == 16, "EmaStateLowered must be 16 bytes");
static_assert(offsetof(EmaStateLowered, value) == 0, "EmaStateLowered.value offset");
static_assert(offsetof(EmaStateLowered, initialized) == 8, "EmaStateLowered.initialized offset");

static_assert(sizeof(LagStateLowered) == 32, "LagStateLowered must be 32 bytes");
static_assert(offsetof(LagStateLowered, buffer) == 0, "LagStateLowered.buffer offset");
static_assert(offsetof(LagStateLowered, head) == 8, "LagStateLowered.head offset");
static_assert(offsetof(LagStateLowered, count) == 16, "LagStateLowered.count offset");
static_assert(offsetof(LagStateLowered, capacity) == 24, "LagStateLowered.capacity offset");

#if defined(__linux__) && defined(__x86_64__)
static_assert(sizeof(long double) == 16, "long double must be 16 bytes on x86_64 SysV");
static_assert(sizeof(RollingStdStateLowered) == 96, "RollingStdStateLowered must be 96 bytes");
static_assert(offsetof(RollingStdStateLowered, buffer) == 0, "RollingStdStateLowered.buffer offset");
static_assert(offsetof(RollingStdStateLowered, capacity) == 8, "RollingStdStateLowered.capacity offset");
static_assert(offsetof(RollingStdStateLowered, head) == 16, "RollingStdStateLowered.head offset");
static_assert(offsetof(RollingStdStateLowered, count) == 24, "RollingStdStateLowered.count offset");
static_assert(offsetof(RollingStdStateLowered, sum) == 32, "RollingStdStateLowered.sum offset");
static_assert(offsetof(RollingStdStateLowered, mean) == 48, "RollingStdStateLowered.mean offset");
static_assert(offsetof(RollingStdStateLowered, m2) == 64, "RollingStdStateLowered.m2 offset");
static_assert(offsetof(RollingStdStateLowered, slides_since_refresh) == 80, "RollingStdStateLowered.slides_since_refresh offset");
#endif

static_assert(sizeof(MonoDequeEntryLowered) == 16, "MonoDequeEntryLowered must be 16 bytes");
static_assert(offsetof(MonoDequeEntryLowered, tick_index) == 0, "MonoDequeEntryLowered.tick_index offset");
static_assert(offsetof(MonoDequeEntryLowered, value) == 8, "MonoDequeEntryLowered.value offset");
static_assert(sizeof(RollingMinMaxStateLowered) == 40, "RollingMinMaxStateLowered must be 40 bytes");
static_assert(offsetof(RollingMinMaxStateLowered, buf) == 0, "RollingMinMaxStateLowered.buf offset");
static_assert(offsetof(RollingMinMaxStateLowered, head) == 8, "RollingMinMaxStateLowered.head offset");
static_assert(offsetof(RollingMinMaxStateLowered, count) == 16, "RollingMinMaxStateLowered.count offset");
static_assert(offsetof(RollingMinMaxStateLowered, cap) == 24, "RollingMinMaxStateLowered.cap offset");
static_assert(offsetof(RollingMinMaxStateLowered, idx) == 32, "RollingMinMaxStateLowered.idx offset");

static_assert(sizeof(CrossStateLowered) == 24, "CrossStateLowered must be 24 bytes");
static_assert(offsetof(CrossStateLowered, prev_a) == 0, "CrossStateLowered.prev_a offset");
static_assert(offsetof(CrossStateLowered, prev_b) == 8, "CrossStateLowered.prev_b offset");
static_assert(offsetof(CrossStateLowered, initialized) == 16, "CrossStateLowered.initialized offset");

static_assert(sizeof(Kalman1dStateLowered) == 24, "Kalman1dStateLowered must be 24 bytes");
static_assert(offsetof(Kalman1dStateLowered, x_hat) == 0, "Kalman1dStateLowered.x_hat offset");
static_assert(offsetof(Kalman1dStateLowered, p) == 8, "Kalman1dStateLowered.p offset");
static_assert(offsetof(Kalman1dStateLowered, initialized) == 16, "Kalman1dStateLowered.initialized offset");

static_assert(sizeof(VwapStateLowered) == 56, "VwapStateLowered must be 56 bytes");
static_assert(offsetof(VwapStateLowered, price_buf) == 0, "VwapStateLowered.price_buf offset");
static_assert(offsetof(VwapStateLowered, vol_buf) == 8, "VwapStateLowered.vol_buf offset");
static_assert(offsetof(VwapStateLowered, head) == 16, "VwapStateLowered.head offset");
static_assert(offsetof(VwapStateLowered, count) == 24, "VwapStateLowered.count offset");
static_assert(offsetof(VwapStateLowered, capacity) == 32, "VwapStateLowered.capacity offset");
static_assert(offsetof(VwapStateLowered, sum_pv) == 40, "VwapStateLowered.sum_pv offset");
static_assert(offsetof(VwapStateLowered, sum_vol) == 48, "VwapStateLowered.sum_vol offset");

static_assert(sizeof(RollingPairStateLowered) == 128, "RollingPairStateLowered must be 128 bytes");
static_assert(alignof(RollingPairStateLowered) == 16, "RollingPairStateLowered must be 16-byte aligned");
static_assert(offsetof(RollingPairStateLowered, x_buf) == 0, "RollingPairStateLowered.x_buf offset");
static_assert(offsetof(RollingPairStateLowered, y_buf) == 8, "RollingPairStateLowered.y_buf offset");
static_assert(offsetof(RollingPairStateLowered, head) == 16, "RollingPairStateLowered.head offset");
static_assert(offsetof(RollingPairStateLowered, count) == 24, "RollingPairStateLowered.count offset");
static_assert(offsetof(RollingPairStateLowered, capacity) == 32, "RollingPairStateLowered.capacity offset");
static_assert(offsetof(RollingPairStateLowered, sum_x) == 48, "RollingPairStateLowered.sum_x offset");
static_assert(offsetof(RollingPairStateLowered, sum_y) == 64, "RollingPairStateLowered.sum_y offset");
static_assert(offsetof(RollingPairStateLowered, sum_xy) == 80, "RollingPairStateLowered.sum_xy offset");
static_assert(offsetof(RollingPairStateLowered, sum_xx) == 96, "RollingPairStateLowered.sum_xx offset");
static_assert(offsetof(RollingPairStateLowered, sum_yy) == 112, "RollingPairStateLowered.sum_yy offset");

// The JIT's IR struct for InstrumentState must match the C++ layout exactly:
// alignas(64) on InstrumentState pads the natural 40-byte struct to 64
// bytes so consecutive instruments fall on consecutive cache lines.
// jit_compiler.cpp::EmitMarketFieldLoad mirrors this with a trailing
// `[24 x i8]` padding field; if `sizeof(InstrumentState)` ever changes,
// that IR-struct definition must change too.
static_assert(sizeof(InstrumentState) == 64, "InstrumentState must be 64 bytes (JIT IR layout depends on it)");
static_assert(alignof(InstrumentState) == 64, "InstrumentState must be 64-byte aligned");
static_assert(offsetof(InstrumentState, bid) == 0, "InstrumentState.bid offset");
static_assert(offsetof(InstrumentState, ask) == 8, "InstrumentState.ask offset");
static_assert(offsetof(InstrumentState, last_price) == 16, "InstrumentState.last_price offset");
static_assert(offsetof(InstrumentState, volume) == 24, "InstrumentState.volume offset");
static_assert(offsetof(InstrumentState, last_update_ns) == 32, "InstrumentState.last_update_ns offset");

namespace {

constexpr std::size_t RingStatsRefreshInterval(std::size_t capacity) {
#if defined(_MSC_VER)
  // MSVC exposes only 64-bit `long double`, so the incremental Welford state
  // accumulates more drift on long, low-variance windows. Refresh every slide
  // on that toolchain to keep the incremental path anchored to the buffer.
  (void)capacity;
  return 1;
#else
  return capacity;
#endif
}

void MonoInit(MonoDequeState& st, std::size_t period) {
  const std::size_t needed_cap = period + 1;
  if (st.cap == needed_cap) return;
  st.buf.assign(needed_cap, {0, 0.0});
  st.head = 0;
  st.count = 0;
  st.cap = needed_cap;
}

std::size_t ParsePreparedPeriod(const FunctionCall& fn) {
  if (fn.args.size() < 2) return 0;
  const auto* period_node = dynamic_cast<const NumberLiteral*>(fn.args[1].get());
  if (period_node == nullptr) return 0;
  const int period = static_cast<int>(period_node->value);
  if (period <= 0 || std::fabs(period_node->value - static_cast<double>(period)) > 1e-12) return 0;
  return static_cast<std::size_t>(period);
}

bool MonoEmpty(const MonoDequeState& st) { return st.count == 0; }

std::size_t MonoTailIndex(const MonoDequeState& st) {
  return (st.head + st.count - 1) % st.cap;
}

std::pair<std::size_t, double>& MonoFront(MonoDequeState& st) { return st.buf[st.head]; }
const std::pair<std::size_t, double>& MonoFront(const MonoDequeState& st) { return st.buf[st.head]; }
std::pair<std::size_t, double>& MonoBack(MonoDequeState& st) { return st.buf[MonoTailIndex(st)]; }

void MonoPopFront(MonoDequeState& st) {
  st.head = (st.head + 1) % st.cap;
  --st.count;
}

void MonoPopBack(MonoDequeState& st) { --st.count; }

void MonoPushBack(MonoDequeState& st, std::pair<std::size_t, double> v) {
  const std::size_t write_idx = (st.head + st.count) % st.cap;
  st.buf[write_idx] = v;
  ++st.count;
}

std::size_t CurrentNodeCapacity(const SignalContext& ctx) {
  return std::max(
      {ctx.ema_states.size(),
       ctx.sma_states.size(),
       ctx.rolling_std_states.size(),
       ctx.zscore_states.size(),
       ctx.vwap_states.size(),
       ctx.lag_states.size(),
       ctx.cross_states.size(),
       ctx.rolling_min_deques.size(),
       ctx.rolling_max_deques.size(),
       ctx.rolling_corr_states.size(),
       ctx.rolling_beta_states.size(),
       ctx.kalman1d_states.size()});
}

void ResizeSensitivityStorage(SignalContext& ctx, std::size_t node_capacity, std::size_t param_count) {
  ctx.gradient_param_count = param_count;
  const std::size_t total = node_capacity * param_count;
  ctx.ema_sensitivity_states.resize(total);
  ctx.sma_sensitivity_states.resize(total);
  ctx.lag_sensitivity_states.resize(total);
  ctx.rolling_std_sensitivity_states.resize(total);
  ctx.zscore_sensitivity_states.resize(total);
  ctx.rolling_corr_sensitivity_states.resize(total);
  ctx.rolling_beta_sensitivity_states.resize(total);
  ctx.kalman1d_sensitivity_states.resize(total);
  ctx.rolling_min_sensitivity_states.resize(total);
  ctx.rolling_max_sensitivity_states.resize(total);
}

void ResetSensitivityStorage(SignalContext& ctx) {
  for (auto& st : ctx.ema_sensitivity_states) st = EmaSensitivityState{};
  for (auto& st : ctx.sma_sensitivity_states) st = RingStatsState{};
  for (auto& st : ctx.lag_sensitivity_states) st = LagState{};
  for (auto& st : ctx.rolling_std_sensitivity_states) st = RollingStdSensitivityState{};
  for (auto& st : ctx.zscore_sensitivity_states) st = RollingStdSensitivityState{};
  for (auto& st : ctx.rolling_corr_sensitivity_states) st = RollingPairSensitivityState{};
  for (auto& st : ctx.rolling_beta_sensitivity_states) st = RollingPairSensitivityState{};
  for (auto& st : ctx.kalman1d_sensitivity_states) st = Kalman1dSensitivityState{};
  for (auto& st : ctx.rolling_min_sensitivity_states) st = LagState{};
  for (auto& st : ctx.rolling_max_sensitivity_states) st = LagState{};
}

std::size_t GradientSlot(const SignalContext& ctx, std::size_t node_id, std::size_t param_id) {
  if (ctx.gradient_param_count == 0) {
    throw std::runtime_error("Gradient slot access without initialized parameter storage");
  }
  return node_id * ctx.gradient_param_count + param_id;
}

std::size_t CheckedGradientParam(const SignalContext& ctx, std::int64_t param_id) {
  if (param_id < 0 || static_cast<std::size_t>(param_id) >= ctx.gradient_param_count) {
    throw std::runtime_error("Gradient param_id out of range");
  }
  return static_cast<std::size_t>(param_id);
}

long double RollingStdRefreshSensitivity(RollingStdSensitivityState& st, const RingStatsState& primal) {
  const long double n = static_cast<long double>(primal.count);
  long double mean_prime = 0.0L;
  for (std::size_t i = 0; i < primal.count; ++i) {
    mean_prime += st.buffer[i];
  }
  mean_prime /= n;
  long double m2_prime = 0.0L;
  for (std::size_t i = 0; i < primal.count; ++i) {
    const long double d = static_cast<long double>(primal.buffer[i]) - primal.mean;
    const long double d_prime = st.buffer[i] - mean_prime;
    m2_prime += 2.0L * d * d_prime;
  }
  st.mean = mean_prime;
  st.m2 = m2_prime;
  st.slides_since_refresh = 0;
  return mean_prime;
}

double RollingStdOrZscoreGradStep(
    SignalContext& ctx,
    std::size_t node_id,
    double x,
    double x_grad,
    std::size_t period,
    std::size_t param_id,
    bool zscore,
    double* grad_out) {
  RingStatsState& primal = zscore ? ctx.zscore_states[node_id] : ctx.rolling_std_states[node_id];
  RollingStdSensitivityState& grad =
      zscore ? ctx.zscore_sensitivity_states[GradientSlot(ctx, node_id, param_id)]
             : ctx.rolling_std_sensitivity_states[GradientSlot(ctx, node_id, param_id)];

  if (primal.capacity != period) {
    primal.buffer.assign(period, 0.0);
    primal.capacity = period;
    primal.head = 0;
    primal.count = 0;
    primal.sum = 0.0L;
    primal.mean = 0.0L;
    primal.m2 = 0.0L;
    primal.slides_since_refresh = 0;
  }
  if (grad.capacity != primal.capacity) {
    grad.buffer.assign(primal.capacity, 0.0L);
    grad.capacity = primal.capacity;
    grad.head = 0;
    grad.count = 0;
    grad.mean = 0.0L;
    grad.m2 = 0.0L;
    grad.slides_since_refresh = 0;
  }

  bool slid = false;
  if (primal.count == primal.capacity) {
    const double old = primal.buffer[primal.head];
    const long double old_prime = grad.buffer[grad.head];
    primal.sum -= old;
    if (primal.capacity <= 1) {
      primal.mean = static_cast<long double>(x);
      primal.m2 = 0.0L;
      grad.mean = static_cast<long double>(x_grad);
      grad.m2 = 0.0L;
    } else {
      const long double n = static_cast<long double>(primal.count);
      const long double delta_r = static_cast<long double>(old) - primal.mean;
      const long double delta_r_prime = old_prime - grad.mean;
      const long double mean_r = primal.mean - delta_r / (n - 1.0L);
      const long double mean_r_prime = grad.mean - delta_r_prime / (n - 1.0L);
      const long double m2_r =
          primal.m2 - delta_r * (static_cast<long double>(old) - mean_r);
      const long double m2_r_prime =
          grad.m2 - delta_r_prime * (static_cast<long double>(old) - mean_r) -
          delta_r * (old_prime - mean_r_prime);

      const long double delta_a = static_cast<long double>(x) - mean_r;
      const long double delta_a_prime = static_cast<long double>(x_grad) - mean_r_prime;
      primal.mean = mean_r + delta_a / n;
      grad.mean = mean_r_prime + delta_a_prime / n;
      primal.m2 = m2_r + delta_a * (static_cast<long double>(x) - primal.mean);
      grad.m2 = m2_r_prime +
                delta_a_prime * (static_cast<long double>(x) - primal.mean) +
                delta_a * (static_cast<long double>(x_grad) - grad.mean);
      if (grad.m2 < 0.0L) grad.m2 = 0.0L;
      if (primal.m2 < 0.0L) primal.m2 = 0.0L;
    }
    slid = true;
  } else {
    ++primal.count;
    ++grad.count;
    const long double n = static_cast<long double>(primal.count);
    const long double delta = static_cast<long double>(x) - primal.mean;
    const long double delta_prime = static_cast<long double>(x_grad) - grad.mean;
    primal.mean += delta / n;
    grad.mean += delta_prime / n;
    primal.m2 += delta * (static_cast<long double>(x) - primal.mean);
    grad.m2 += delta_prime * (static_cast<long double>(x) - primal.mean) +
               delta * (static_cast<long double>(x_grad) - grad.mean);
  }

  if (!grad.buffer.empty()) {
    primal.buffer[primal.head] = x;
    primal.sum += x;
    grad.buffer[grad.head] = static_cast<long double>(x_grad);
    grad.head = (grad.head + 1) % grad.capacity;
    primal.head = (primal.head + 1) % primal.capacity;
  }
  if (slid) {
    ++primal.slides_since_refresh;
    ++grad.slides_since_refresh;
    if (grad.slides_since_refresh >= grad.capacity && grad.capacity > 1) {
      const long double n = static_cast<long double>(primal.count);
      long double sum = 0.0L;
      for (std::size_t i = 0; i < primal.count; ++i) {
        sum += static_cast<long double>(primal.buffer[i]);
      }
      primal.mean = sum / n;
      long double ss = 0.0L;
      for (std::size_t i = 0; i < primal.count; ++i) {
        const long double d = static_cast<long double>(primal.buffer[i]) - primal.mean;
        ss += d * d;
      }
      primal.m2 = ss < 0.0L ? 0.0L : ss;
      primal.slides_since_refresh = 0;
      RollingStdRefreshSensitivity(grad, primal);
    }
  }

  if (!RingStatsFull(primal)) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double stddev = RingStatsStddevSample(primal);
  if (!zscore) {
    if (primal.count < 2) {
      *grad_out = std::numeric_limits<double>::quiet_NaN();
      return std::numeric_limits<double>::quiet_NaN();
    }
    const long double n = static_cast<long double>(primal.count);
    const long double var_prime = grad.m2 / (n - 1.0L);
    *grad_out = std::isnan(stddev)
                    ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(var_prime / (2.0L * static_cast<long double>(stddev)));
    return stddev;
  }

  const double mean = RingStatsMean(primal);
  if (std::isnan(stddev) || std::fabs(stddev) < 1e-18) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const long double n = static_cast<long double>(primal.count);
  const long double var_prime = grad.m2 / (n - 1.0L);
  const long double std_prime = var_prime / (2.0L * static_cast<long double>(stddev));
  *grad_out =
      (x_grad - static_cast<double>(grad.mean)) / stddev -
      (x - mean) * static_cast<double>(std_prime) / (stddev * stddev);
  return (x - mean) / stddev;
}

}  // namespace

std::size_t SymbolTable::RegisterOrGetId(const std::string& symbol) {
  auto it = symbol_to_id_.find(symbol);
  if (it != symbol_to_id_.end()) {
    return it->second;
  }
  if (next_id_ >= kMaxInstruments) {
    throw std::runtime_error("Symbol table exceeded kMaxInstruments");
  }
  const std::size_t assigned = next_id_;
  symbol_to_id_[symbol] = assigned;
  ++next_id_;
  return assigned;
}

std::size_t SymbolTable::LookupId(const std::string& symbol) const {
  auto it = symbol_to_id_.find(symbol);
  if (it == symbol_to_id_.end()) {
    throw std::runtime_error("Unknown symbol: " + symbol);
  }
  return it->second;
}

namespace {

// Rolling Welford update (West 1979). `state.count` reflects the current
// number of samples in the window AFTER this call returns. The two
// shapes are:
//
//   1. Add path (window still filling): `count` increases by 1.
//        delta = sample - mean
//        mean += delta / count
//        m2   += delta * (sample - mean)
//
//   2. Slide path (window full, replacing `old` with `sample`): `count`
//      stays at capacity. Conceptually a `remove(old)` then `add(sample)`:
//
//        // remove (count -> count - 1)
//        delta_r = old - mean
//        mean -= delta_r / (count - 1)
//        m2   -= delta_r * (old - mean)   // using post-remove mean
//
//        // add (count - 1 -> count)
//        delta_a = sample - mean
//        mean += delta_a / count
//        m2   += delta_a * (sample - mean) // using post-add mean
//
// When `capacity == 1` the slide path would divide by zero on remove;
// in that case we just snap mean to the new sample and zero m2 (the
// sample stddev is NaN for a window of one element either way, gated
// by the `count < 2` check in `RingStatsStddevSample`).
void RingStatsUpdateMeanM2OnAdd(RingStatsState& state, double sample) {
  const long double x = static_cast<long double>(sample);
  const long double n = static_cast<long double>(state.count);
  const long double delta = x - state.mean;
  state.mean += delta / n;
  const long double delta2 = x - state.mean;
  state.m2 += delta * delta2;
}

void RingStatsUpdateMeanM2OnSlide(RingStatsState& state, double old_sample, double new_sample) {
  if (state.capacity <= 1) {
    state.mean = static_cast<long double>(new_sample);
    state.m2 = 0.0L;
    return;
  }
  const long double n = static_cast<long double>(state.count);
  // Phase 1: remove old (n -> n-1).
  {
    const long double x_old = static_cast<long double>(old_sample);
    const long double delta = x_old - state.mean;
    state.mean -= delta / (n - 1.0L);
    state.m2 -= delta * (x_old - state.mean);
  }
  // Phase 2: add new (n-1 -> n).
  {
    const long double x_new = static_cast<long double>(new_sample);
    const long double delta = x_new - state.mean;
    state.mean += delta / n;
    state.m2 += delta * (x_new - state.mean);
  }
  // Welford m2 should be >= 0 by construction; floating-point can leak
  // a tiny negative when the true variance is ~0. Clamp to 0 so callers
  // don't have to handle the case.
  if (state.m2 < 0.0L) state.m2 = 0.0L;
}

// Recompute `mean` and `m2` from the buffer in long double. This is
// the same arithmetic as `RingStatsStddevSampleTwoPassReference` but
// it writes the results back into the incremental accumulators, so
// the next slide step starts from a freshly-computed (zero-drift)
// reference instead of from one that has been updated incrementally
// for many slides.
//
// Called from `RingStatsPush` / `RingStatsPushPrepared` AFTER the
// new sample has been written to the buffer, when
// `slides_since_refresh` reaches `capacity`. The recompute itself is
// O(capacity); spacing it `capacity` slides apart keeps the
// amortized per-slide cost O(1) while bounding rolling-Welford
// roundoff drift to ONE window's worth of long-double roundoff
// regardless of stream length.
void RingStatsRefreshMeanM2FromBuffer(RingStatsState& state) {
  const long double n = static_cast<long double>(state.count);
  long double sum = 0.0L;
  for (std::size_t i = 0; i < state.count; ++i) {
    sum += static_cast<long double>(state.buffer[i]);
  }
  const long double mean = sum / n;
  long double ss = 0.0L;
  for (std::size_t i = 0; i < state.count; ++i) {
    const long double d = static_cast<long double>(state.buffer[i]) - mean;
    ss += d * d;
  }
  state.mean = mean;
  state.m2 = ss < 0.0L ? 0.0L : ss;
  state.slides_since_refresh = 0;
}

}  // namespace

void RingStatsPush(RingStatsState& state, std::size_t period, double sample) {
  if (period == 0) {
    throw std::runtime_error("RingStatsPush requires period > 0");
  }
  if (state.capacity != period) {
    state.buffer.assign(period, 0.0);
    state.capacity = period;
    state.head = 0;
    state.count = 0;
    state.sum = 0.0L;
    state.mean = 0.0L;
    state.m2 = 0.0L;
    state.slides_since_refresh = 0;
  }

  bool slid = false;
  if (state.count == state.capacity) {
    const double old = state.buffer[state.head];
    state.sum -= old;
    RingStatsUpdateMeanM2OnSlide(state, old, sample);
    slid = true;
  } else {
    ++state.count;
    RingStatsUpdateMeanM2OnAdd(state, sample);
  }

  state.buffer[state.head] = sample;
  state.sum += sample;
  state.head = (state.head + 1) % state.capacity;

#if defined(_MSC_VER)
  if (state.capacity > 1) {
    RingStatsRefreshMeanM2FromBuffer(state);
    return;
  }
#endif
  if (slid) {
    ++state.slides_since_refresh;
    if (state.slides_since_refresh >= RingStatsRefreshInterval(state.capacity) &&
        state.capacity > 1) {
      RingStatsRefreshMeanM2FromBuffer(state);
    }
  }
}

void RingStatsPushPrepared(RingStatsState& state, double sample) {
  if (state.capacity == 0 || state.buffer.empty()) {
    throw std::runtime_error("RingStatsPushPrepared called before state prewarm");
  }
  bool slid = false;
  if (state.count == state.capacity) {
    const double old = state.buffer[state.head];
    state.sum -= old;
    RingStatsUpdateMeanM2OnSlide(state, old, sample);
    slid = true;
  } else {
    ++state.count;
    RingStatsUpdateMeanM2OnAdd(state, sample);
  }
  state.buffer[state.head] = sample;
  state.sum += sample;
  state.head = (state.head + 1) % state.capacity;

#if defined(_MSC_VER)
  if (state.capacity > 1) {
    RingStatsRefreshMeanM2FromBuffer(state);
    return;
  }
#endif
  if (slid) {
    ++state.slides_since_refresh;
    if (state.slides_since_refresh >= RingStatsRefreshInterval(state.capacity) &&
        state.capacity > 1) {
      RingStatsRefreshMeanM2FromBuffer(state);
    }
  }
}

double RingStatsMean(const RingStatsState& state) {
  if (state.count == 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return static_cast<double>(state.sum / static_cast<long double>(state.count));
}

double RingStatsStddevSample(const RingStatsState& state) {
  if (state.count < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // O(1) Welford readout. `m2` is the sum of squared deviations from
  // the running mean, maintained incrementally on every push, so the
  // sample variance is just `m2 / (n - 1)`. The two-pass long-double
  // formula is kept as `RingStatsStddevSampleTwoPassReference` and
  // gated by `welford_stddev_parity_test` as the numerical oracle.
  const long double n = static_cast<long double>(state.count);
  long double var = state.m2 / (n - 1.0L);
  // Welford's m2 is non-negative by construction; the slide-path clamp
  // covers floating-point underflow into a tiny negative. This guard is
  // defensive in case future callers mutate `m2` directly.
  const long double scale = 1.0L + state.m2;
  if (var < 0.0L && std::fabs(var) <= 1e-15L * scale) {
    var = 0.0L;
  }
  if (var < 0.0L) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::sqrt(static_cast<double>(var));
}

double RingStatsStddevSampleTwoPassReference(const RingStatsState& state) {
  if (state.count < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const long double n = static_cast<long double>(state.count);
  long double sum = 0.0L;
  for (std::size_t i = 0; i < state.count; ++i) {
    sum += static_cast<long double>(state.buffer[i]);
  }
  const long double mean = sum / n;
  long double ss = 0.0L;
  for (std::size_t i = 0; i < state.count; ++i) {
    const long double d = static_cast<long double>(state.buffer[i]) - mean;
    ss += d * d;
  }
  long double var = ss / (n - 1.0L);
  const long double scale = 1.0L + ss;
  if (var < 0.0L && std::fabs(var) <= 1e-15L * scale) {
    var = 0.0L;
  }
  if (var < 0.0L) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::sqrt(static_cast<double>(var));
}

bool RingStatsFull(const RingStatsState& state) { return state.count == state.capacity && state.capacity > 0; }

void VwapPush(VwapState& state, std::size_t period, double price, double volume) {
  if (period == 0) {
    throw std::runtime_error("VwapPush requires period > 0");
  }
  if (state.capacity != period) {
    state.price_buf.assign(period, 0.0);
    state.vol_buf.assign(period, 0.0);
    state.capacity = period;
    state.head = 0;
    state.count = 0;
    state.sum_pv = 0.0L;
    state.sum_vol = 0.0L;
  }
  if (state.count == state.capacity) {
    const double old_p = state.price_buf[state.head];
    const double old_v = state.vol_buf[state.head];
    state.sum_pv -= static_cast<long double>(old_p) * static_cast<long double>(old_v);
    state.sum_vol -= static_cast<long double>(old_v);
  } else {
    ++state.count;
  }
  state.price_buf[state.head] = price;
  state.vol_buf[state.head] = volume;
  state.sum_pv += static_cast<long double>(price) * static_cast<long double>(volume);
  state.sum_vol += static_cast<long double>(volume);
  state.head = (state.head + 1) % state.capacity;
}

void VwapPushPrepared(VwapState& state, double price, double volume) {
  if (state.capacity == 0 || state.price_buf.empty() || state.vol_buf.empty()) {
    throw std::runtime_error("VwapPushPrepared called before state prewarm");
  }
  if (state.count == state.capacity) {
    const double old_p = state.price_buf[state.head];
    const double old_v = state.vol_buf[state.head];
    state.sum_pv -= static_cast<long double>(old_p) * static_cast<long double>(old_v);
    state.sum_vol -= static_cast<long double>(old_v);
  } else {
    ++state.count;
  }
  state.price_buf[state.head] = price;
  state.vol_buf[state.head] = volume;
  state.sum_pv += static_cast<long double>(price) * static_cast<long double>(volume);
  state.sum_vol += static_cast<long double>(volume);
  state.head = (state.head + 1) % state.capacity;
}

double VwapValue(const VwapState& state) {
  if (state.count == 0 || state.sum_vol == 0.0L) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return static_cast<double>(state.sum_pv / state.sum_vol);
}

bool VwapFull(const VwapState& state) { return state.count == state.capacity && state.capacity > 0; }

void LagPush(LagState& state, std::size_t period, double sample) {
  if (period == 0) {
    throw std::runtime_error("LagPush requires period > 0");
  }
  if (state.capacity != period) {
    state.buffer.assign(period, 0.0);
    state.capacity = period;
    state.head = 0;
    state.count = 0;
  }
  state.buffer[state.head] = sample;
  state.head = (state.head + 1) % state.capacity;
  if (state.count < state.capacity) ++state.count;
}

void LagPushPrepared(LagState& state, double sample) {
  if (state.capacity == 0 || state.buffer.empty()) {
    throw std::runtime_error("LagPushPrepared called before state prewarm");
  }
  state.buffer[state.head] = sample;
  state.head = (state.head + 1) % state.capacity;
  if (state.count < state.capacity) ++state.count;
}

double LagValue(const LagState& state) {
  if (state.capacity == 0 || state.count < state.capacity) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return state.buffer[state.head];
}

// P7 paired-series rolling stats. Standard O(1)-per-update running-sum
// recurrence, identical structurally to RingStatsPush.
void RollingPairPush(RollingPairState& state, std::size_t period, double x, double y) {
  if (period == 0) {
    throw std::runtime_error("RollingPairPush requires period > 0");
  }
  if (state.capacity != period) {
    state.x_buf.assign(period, 0.0);
    state.y_buf.assign(period, 0.0);
    state.capacity = period;
    state.head = 0;
    state.count = 0;
    state.sum_x = 0.0L;
    state.sum_y = 0.0L;
    state.sum_xy = 0.0L;
    state.sum_xx = 0.0L;
    state.sum_yy = 0.0L;
  }
  if (state.count == state.capacity) {
    const double ox = state.x_buf[state.head];
    const double oy = state.y_buf[state.head];
    state.sum_x  -= ox;
    state.sum_y  -= oy;
    state.sum_xy -= static_cast<long double>(ox) * static_cast<long double>(oy);
    state.sum_xx -= static_cast<long double>(ox) * static_cast<long double>(ox);
    state.sum_yy -= static_cast<long double>(oy) * static_cast<long double>(oy);
  } else {
    ++state.count;
  }
  state.x_buf[state.head] = x;
  state.y_buf[state.head] = y;
  state.sum_x  += x;
  state.sum_y  += y;
  state.sum_xy += static_cast<long double>(x) * static_cast<long double>(y);
  state.sum_xx += static_cast<long double>(x) * static_cast<long double>(x);
  state.sum_yy += static_cast<long double>(y) * static_cast<long double>(y);
  state.head = (state.head + 1) % state.capacity;
}

bool RollingPairFull(const RollingPairState& state) {
  return state.count == state.capacity && state.capacity > 0;
}

double RollingPairCorrelation(const RollingPairState& state) {
  if (!RollingPairFull(state)) return std::numeric_limits<double>::quiet_NaN();
  const long double n = static_cast<long double>(state.count);
  // cov_unnorm = sum_xy - sum_x * sum_y / n. Same for var_x, var_y.
  const long double cov_unnorm = state.sum_xy - state.sum_x * state.sum_y / n;
  const long double var_x_unnorm = state.sum_xx - state.sum_x * state.sum_x / n;
  const long double var_y_unnorm = state.sum_yy - state.sum_y * state.sum_y / n;
  // Both unnormalized vars are sums-of-squared-deviations; they should be >= 0,
  // but catastrophic cancellation can drive them slightly negative.
  if (var_x_unnorm <= 0.0L || var_y_unnorm <= 0.0L) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const long double denom = std::sqrt(var_x_unnorm * var_y_unnorm);
  if (denom == 0.0L) return std::numeric_limits<double>::quiet_NaN();
  long double r = cov_unnorm / denom;
  // Clamp into [-1, 1] to absorb floating-point overshoot at the boundary.
  if (r > 1.0L) r = 1.0L;
  if (r < -1.0L) r = -1.0L;
  return static_cast<double>(r);
}

double RollingPairBeta(const RollingPairState& state) {
  if (!RollingPairFull(state)) return std::numeric_limits<double>::quiet_NaN();
  const long double n = static_cast<long double>(state.count);
  const long double cov_unnorm = state.sum_xy - state.sum_x * state.sum_y / n;
  const long double var_x_unnorm = state.sum_xx - state.sum_x * state.sum_x / n;
  if (var_x_unnorm <= 0.0L) return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(cov_unnorm / var_x_unnorm);
}

double Kalman1dStep(Kalman1dState& state, double x, double q, double r) {
  // Scalar Kalman: predict (x_hat_p = x_hat; p_p = p + q), then update
  // (K = p_p / (p_p + r); x_hat = x_hat_p + K * (x - x_hat_p);
  //  p = (1 - K) * p_p). On first call we initialize x_hat = x, p = r so
  // the first posterior equals the measurement.
  if (!state.initialized) {
    state.x_hat = x;
    state.p = r;
    state.q = q;
    state.r = r;
    state.initialized = true;
    return state.x_hat;
  }
  const double p_pred = state.p + q;
  const double denom = p_pred + r;
  // r is parameter; we expect r > 0. Guard against degenerate (q == r == 0)
  // configurations to keep parity well-defined.
  if (denom <= 0.0) {
    return state.x_hat;
  }
  const double k_gain = p_pred / denom;
  state.x_hat = state.x_hat + k_gain * (x - state.x_hat);
  double p_new = (1.0 - k_gain) * p_pred;
  if (p_new < 0.0) p_new = 0.0;
  state.p = p_new;
  state.q = q;
  state.r = r;
  return state.x_hat;
}

void EnsureNodeCapacity(SignalContext& ctx, std::size_t node_id) {
  const std::size_t needed = node_id + 1;
  if (ctx.ema_states.size() < needed) ctx.ema_states.resize(needed);
  if (ctx.sma_states.size() < needed) ctx.sma_states.resize(needed);
  if (ctx.rolling_std_states.size() < needed) ctx.rolling_std_states.resize(needed);
  if (ctx.zscore_states.size() < needed) ctx.zscore_states.resize(needed);
  if (ctx.vwap_states.size() < needed) ctx.vwap_states.resize(needed);
  if (ctx.lag_states.size() < needed) ctx.lag_states.resize(needed);
  if (ctx.cross_states.size() < needed) ctx.cross_states.resize(needed);
  if (ctx.rolling_min_deques.size() < needed) ctx.rolling_min_deques.resize(needed);
  if (ctx.rolling_max_deques.size() < needed) ctx.rolling_max_deques.resize(needed);
  if (ctx.rolling_min_indices.size() < needed) ctx.rolling_min_indices.resize(needed);
  if (ctx.rolling_max_indices.size() < needed) ctx.rolling_max_indices.resize(needed);

  if (ctx.sma_lowered.size() < needed) ctx.sma_lowered.resize(needed, SmaStateLowered{});
  if (ctx.sma_lowered_buffers.size() < needed) ctx.sma_lowered_buffers.resize(needed);
  if (ctx.ema_lowered.size() < needed) ctx.ema_lowered.resize(needed, EmaStateLowered{});
  if (ctx.lag_lowered.size() < needed) ctx.lag_lowered.resize(needed, LagStateLowered{});
  if (ctx.lag_lowered_buffers.size() < needed) ctx.lag_lowered_buffers.resize(needed);
  if (ctx.rolling_std_lowered.size() < needed) ctx.rolling_std_lowered.resize(needed, RollingStdStateLowered{});
  if (ctx.rolling_std_lowered_buffers.size() < needed) ctx.rolling_std_lowered_buffers.resize(needed);
  if (ctx.zscore_lowered.size() < needed) ctx.zscore_lowered.resize(needed, RollingStdStateLowered{});
  if (ctx.zscore_lowered_buffers.size() < needed) ctx.zscore_lowered_buffers.resize(needed);
  if (ctx.rolling_min_lowered.size() < needed) ctx.rolling_min_lowered.resize(needed, RollingMinMaxStateLowered{});
  if (ctx.rolling_min_lowered_buffers.size() < needed) ctx.rolling_min_lowered_buffers.resize(needed);
  if (ctx.rolling_max_lowered.size() < needed) ctx.rolling_max_lowered.resize(needed, RollingMinMaxStateLowered{});
  if (ctx.rolling_max_lowered_buffers.size() < needed) ctx.rolling_max_lowered_buffers.resize(needed);
  if (ctx.cross_lowered.size() < needed) ctx.cross_lowered.resize(needed, CrossStateLowered{});
  if (ctx.kalman1d_lowered.size() < needed) ctx.kalman1d_lowered.resize(needed, Kalman1dStateLowered{});
  if (ctx.vwap_lowered.size() < needed) ctx.vwap_lowered.resize(needed, VwapStateLowered{});
  if (ctx.vwap_price_lowered_buffers.size() < needed) ctx.vwap_price_lowered_buffers.resize(needed);
  if (ctx.vwap_vol_lowered_buffers.size() < needed) ctx.vwap_vol_lowered_buffers.resize(needed);
  if (ctx.rolling_corr_lowered.size() < needed) ctx.rolling_corr_lowered.resize(needed, RollingPairStateLowered{});
  if (ctx.rolling_corr_x_lowered_buffers.size() < needed) ctx.rolling_corr_x_lowered_buffers.resize(needed);
  if (ctx.rolling_corr_y_lowered_buffers.size() < needed) ctx.rolling_corr_y_lowered_buffers.resize(needed);
  if (ctx.rolling_beta_lowered.size() < needed) ctx.rolling_beta_lowered.resize(needed, RollingPairStateLowered{});
  if (ctx.rolling_beta_x_lowered_buffers.size() < needed) ctx.rolling_beta_x_lowered_buffers.resize(needed);
  if (ctx.rolling_beta_y_lowered_buffers.size() < needed) ctx.rolling_beta_y_lowered_buffers.resize(needed);

  // P7 op state.
  if (ctx.rolling_corr_states.size() < needed) ctx.rolling_corr_states.resize(needed);
  if (ctx.rolling_beta_states.size() < needed) ctx.rolling_beta_states.resize(needed);
  if (ctx.kalman1d_states.size() < needed) ctx.kalman1d_states.resize(needed);
  if (ctx.gradient_param_count > 0) {
    ResizeSensitivityStorage(ctx, needed, ctx.gradient_param_count);
  }
  RefreshLoweredStateBases(ctx);
}

void RefreshLoweredStateBases(SignalContext& ctx) {
  ctx.lowered_bases.sma = ctx.sma_lowered.empty() ? nullptr : ctx.sma_lowered.data();
  ctx.lowered_bases.ema = ctx.ema_lowered.empty() ? nullptr : ctx.ema_lowered.data();
  ctx.lowered_bases.lag = ctx.lag_lowered.empty() ? nullptr : ctx.lag_lowered.data();
  ctx.lowered_bases.rolling_std =
      ctx.rolling_std_lowered.empty() ? nullptr : ctx.rolling_std_lowered.data();
  ctx.lowered_bases.zscore = ctx.zscore_lowered.empty() ? nullptr : ctx.zscore_lowered.data();
  ctx.lowered_bases.rolling_min =
      ctx.rolling_min_lowered.empty() ? nullptr : ctx.rolling_min_lowered.data();
  ctx.lowered_bases.rolling_max =
      ctx.rolling_max_lowered.empty() ? nullptr : ctx.rolling_max_lowered.data();
  ctx.lowered_bases.cross = ctx.cross_lowered.empty() ? nullptr : ctx.cross_lowered.data();
  ctx.lowered_bases.kalman1d =
      ctx.kalman1d_lowered.empty() ? nullptr : ctx.kalman1d_lowered.data();
  ctx.lowered_bases.vwap = ctx.vwap_lowered.empty() ? nullptr : ctx.vwap_lowered.data();
  ctx.lowered_bases.rolling_corr =
      ctx.rolling_corr_lowered.empty() ? nullptr : ctx.rolling_corr_lowered.data();
  ctx.lowered_bases.rolling_beta =
      ctx.rolling_beta_lowered.empty() ? nullptr : ctx.rolling_beta_lowered.data();
}

static_assert(offsetof(SignalContext, lowered_bases) == 0,
              "JIT lowered-base loads assume lowered_bases is the first SignalContext field");

void PrewarmSignalContext(SignalContext& ctx, const SignalDef& signal) {
  std::function<void(const Expr&)> walk = [&](const Expr& expr) {
    if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
      walk(*u->operand);
      return;
    }
    if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
      walk(*b->left);
      walk(*b->right);
      return;
    }
    if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
      walk(*c->condition);
      walk(*c->then_branch);
      walk(*c->else_branch);
      return;
    }
    if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
      if (fn->node_id >= 0) {
        const std::size_t node_id = static_cast<std::size_t>(fn->node_id);
        EnsureNodeCapacity(ctx, node_id);
        const std::size_t period = ParsePreparedPeriod(*fn);
        if (fn->name == "ema" && period > 0) {
          EMAState& st = ctx.ema_states[node_id];
          st.period = static_cast<std::int64_t>(period);
          st.alpha = 2.0 / (static_cast<double>(period) + 1.0);
          // Mirror to lowered-state slot (idempotent on repeated prewarm).
          EmaStateLowered& low = ctx.ema_lowered[node_id];
          low.value = 0.0;
          low.initialized = 0;
        } else if ((fn->name == "sma" || fn->name == "rolling_std" || fn->name == "zscore") && period > 0) {
          RingStatsState* st = nullptr;
          if (fn->name == "sma") st = &ctx.sma_states[node_id];
          if (fn->name == "rolling_std") st = &ctx.rolling_std_states[node_id];
          if (fn->name == "zscore") st = &ctx.zscore_states[node_id];
          if (st->capacity != period) {
            st->buffer.assign(period, 0.0);
            st->capacity = period;
            st->head = 0;
            st->count = 0;
            st->sum = 0.0L;
            st->mean = 0.0L;
            st->m2 = 0.0L;
          }
          // Mirror lowerable ring-based ops into lowered state buffers.
          if (fn->name == "sma") {
            std::vector<double>& buf_owner = ctx.sma_lowered_buffers[node_id];
            if (buf_owner.size() != period) {
              buf_owner.assign(period, 0.0);
            } else {
              std::fill(buf_owner.begin(), buf_owner.end(), 0.0);
            }
            SmaStateLowered& low = ctx.sma_lowered[node_id];
            low.buffer = buf_owner.data();
            low.sum = 0.0;
            low.head = 0;
            low.count = 0;
            low.capacity = static_cast<std::int64_t>(period);
          } else if (fn->name == "rolling_std") {
            std::vector<double>& buf_owner = ctx.rolling_std_lowered_buffers[node_id];
            if (buf_owner.size() != period) {
              buf_owner.assign(period, 0.0);
            } else {
              std::fill(buf_owner.begin(), buf_owner.end(), 0.0);
            }
            RollingStdStateLowered& low = ctx.rolling_std_lowered[node_id];
            low.buffer = buf_owner.data();
            low.capacity = static_cast<std::int64_t>(period);
            low.head = 0;
            low.count = 0;
            low.sum = 0.0L;
            low.mean = 0.0L;
            low.m2 = 0.0L;
            low.slides_since_refresh = 0;
          } else if (fn->name == "zscore") {
            std::vector<double>& buf_owner = ctx.zscore_lowered_buffers[node_id];
            if (buf_owner.size() != period) {
              buf_owner.assign(period, 0.0);
            } else {
              std::fill(buf_owner.begin(), buf_owner.end(), 0.0);
            }
            RollingStdStateLowered& low = ctx.zscore_lowered[node_id];
            low.buffer = buf_owner.data();
            low.capacity = static_cast<std::int64_t>(period);
            low.head = 0;
            low.count = 0;
            low.sum = 0.0L;
            low.mean = 0.0L;
            low.m2 = 0.0L;
            low.slides_since_refresh = 0;
          }
        } else if (fn->name == "vwap" && period > 0) {
          VwapState& st = ctx.vwap_states[node_id];
          if (st.capacity != period) {
            st.price_buf.assign(period, 0.0);
            st.vol_buf.assign(period, 0.0);
            st.capacity = period;
            st.head = 0;
            st.count = 0;
            st.sum_pv = 0.0L;
            st.sum_vol = 0.0L;
          }
          std::vector<double>& price_buf = ctx.vwap_price_lowered_buffers[node_id];
          std::vector<double>& vol_buf = ctx.vwap_vol_lowered_buffers[node_id];
          if (price_buf.size() != period) {
            price_buf.assign(period, 0.0);
          } else {
            std::fill(price_buf.begin(), price_buf.end(), 0.0);
          }
          if (vol_buf.size() != period) {
            vol_buf.assign(period, 0.0);
          } else {
            std::fill(vol_buf.begin(), vol_buf.end(), 0.0);
          }
          VwapStateLowered& low = ctx.vwap_lowered[node_id];
          low.price_buf = price_buf.data();
          low.vol_buf = vol_buf.data();
          low.head = 0;
          low.count = 0;
          low.capacity = static_cast<std::int64_t>(period);
          low.sum_pv = 0.0;
          low.sum_vol = 0.0;
        } else if (fn->name == "lag" && period > 0) {
          LagState& st = ctx.lag_states[node_id];
          if (st.capacity != period) {
            st.buffer.assign(period, 0.0);
            st.capacity = period;
            st.head = 0;
            st.count = 0;
          }
          std::vector<double>& buf_owner = ctx.lag_lowered_buffers[node_id];
          if (buf_owner.size() != period) {
            buf_owner.assign(period, 0.0);
          } else {
            std::fill(buf_owner.begin(), buf_owner.end(), 0.0);
          }
          LagStateLowered& low = ctx.lag_lowered[node_id];
          low.buffer = buf_owner.data();
          low.head = 0;
          low.count = 0;
          low.capacity = static_cast<std::int64_t>(period);
        } else if ((fn->name == "rolling_min" || fn->name == "rolling_max") && period > 0) {
          if (fn->name == "rolling_min") {
            MonoInit(ctx.rolling_min_deques[node_id], period);
            ctx.rolling_min_indices[node_id] = 0;
            std::vector<MonoDequeEntryLowered>& buf_owner = ctx.rolling_min_lowered_buffers[node_id];
            const std::size_t cap = period + 1;
            if (buf_owner.size() != cap) {
              buf_owner.assign(cap, MonoDequeEntryLowered{0, 0.0});
            } else {
              for (auto& e : buf_owner) {
                e.tick_index = 0;
                e.value = 0.0;
              }
            }
            RollingMinMaxStateLowered& low = ctx.rolling_min_lowered[node_id];
            low.buf = buf_owner.data();
            low.head = 0;
            low.count = 0;
            low.cap = static_cast<std::int64_t>(cap);
            low.idx = 0;
          } else {
            MonoInit(ctx.rolling_max_deques[node_id], period);
            ctx.rolling_max_indices[node_id] = 0;
            std::vector<MonoDequeEntryLowered>& buf_owner = ctx.rolling_max_lowered_buffers[node_id];
            const std::size_t cap = period + 1;
            if (buf_owner.size() != cap) {
              buf_owner.assign(cap, MonoDequeEntryLowered{0, 0.0});
            } else {
              for (auto& e : buf_owner) {
                e.tick_index = 0;
                e.value = 0.0;
              }
            }
            RollingMinMaxStateLowered& low = ctx.rolling_max_lowered[node_id];
            low.buf = buf_owner.data();
            low.head = 0;
            low.count = 0;
            low.cap = static_cast<std::int64_t>(cap);
            low.idx = 0;
          }
        } else if ((fn->name == "rolling_corr" || fn->name == "rolling_beta") && fn->args.size() >= 3) {
          // P7: third arg is the period for paired ops. We re-parse here
          // since ParsePreparedPeriod() reads arg[1] but rolling_corr/beta
          // put their period at arg[2].
          if (const auto* per_lit = dynamic_cast<const NumberLiteral*>(fn->args[2].get())) {
            const int p = static_cast<int>(per_lit->value);
            if (p > 0 && std::fabs(per_lit->value - static_cast<double>(p)) < 1e-12) {
              RollingPairState& st = (fn->name == "rolling_corr")
                                         ? ctx.rolling_corr_states[node_id]
                                         : ctx.rolling_beta_states[node_id];
              if (st.capacity != static_cast<std::size_t>(p)) {
                st.x_buf.assign(p, 0.0);
                st.y_buf.assign(p, 0.0);
                st.capacity = static_cast<std::size_t>(p);
                st.head = 0;
                st.count = 0;
                st.sum_x = st.sum_y = st.sum_xy = st.sum_xx = st.sum_yy = 0.0L;
              }
              std::vector<double>& x_buf = (fn->name == "rolling_corr")
                                               ? ctx.rolling_corr_x_lowered_buffers[node_id]
                                               : ctx.rolling_beta_x_lowered_buffers[node_id];
              std::vector<double>& y_buf = (fn->name == "rolling_corr")
                                               ? ctx.rolling_corr_y_lowered_buffers[node_id]
                                               : ctx.rolling_beta_y_lowered_buffers[node_id];
              if (x_buf.size() != static_cast<std::size_t>(p)) {
                x_buf.assign(p, 0.0);
              } else {
                std::fill(x_buf.begin(), x_buf.end(), 0.0);
              }
              if (y_buf.size() != static_cast<std::size_t>(p)) {
                y_buf.assign(p, 0.0);
              } else {
                std::fill(y_buf.begin(), y_buf.end(), 0.0);
              }
              RollingPairStateLowered& low = (fn->name == "rolling_corr")
                                                 ? ctx.rolling_corr_lowered[node_id]
                                                 : ctx.rolling_beta_lowered[node_id];
              low.x_buf = x_buf.data();
              low.y_buf = y_buf.data();
              low.head = 0;
              low.count = 0;
              low.capacity = static_cast<std::int64_t>(p);
              low.sum_x = 0.0L;
              low.sum_y = 0.0L;
              low.sum_xy = 0.0L;
              low.sum_xx = 0.0L;
              low.sum_yy = 0.0L;
            }
          }
        } else if (fn->name == "kalman1d") {
          // No buffer to allocate; just zero the slot so a fresh prewarm
          // restarts the filter from the initial-measurement state.
          ctx.kalman1d_states[node_id] = Kalman1dState{};
          ctx.kalman1d_lowered[node_id] = Kalman1dStateLowered{};
        } else if (fn->name == "cross_above" || fn->name == "cross_below") {
          ctx.cross_states[node_id] = CrossState{};
          ctx.cross_lowered[node_id] = CrossStateLowered{};
        }
      }
      for (const auto& a : fn->args) {
        walk(*a);
      }
      return;
    }
  };
  walk(*signal.body);
  RefreshLoweredStateBases(ctx);
}

void PrewarmSignalContext(MultiSymbolSignalContext& arena, std::uint32_t symbol_id, const SignalDef& signal) {
  PrewarmSignalContext(arena.PerSymbol(symbol_id), signal);
}

void SetStandaloneParameters(SignalContext& ctx, const std::vector<double>& params) {
  ctx.owned_params = params;
  ctx.params = ctx.owned_params.empty() ? nullptr : ctx.owned_params.data();
  ctx.num_params = ctx.owned_params.size();
  ResizeSensitivityStorage(ctx, CurrentNodeCapacity(ctx), ctx.num_params);
  ResetSensitivityStorage(ctx);
}

void EvaluateAllSymbols(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& arena,
    ProgramStepFn fn,
    double* outputs,
    std::size_t outputs_per_symbol) {
  const std::size_t n = per_symbol_market.size();
  if (arena.NumSymbols() != n) {
    throw std::runtime_error("EvaluateAllSymbols: arena size mismatch");
  }
  for (std::size_t s = 0; s < n; ++s) {
    fn(&per_symbol_market[s], &arena, static_cast<std::uint32_t>(s), outputs + s * outputs_per_symbol);
  }
}

double UpdateRollingMin(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample) {
  MonoInit(dq, period);
  while (!MonoEmpty(dq) && MonoBack(dq).second >= sample) {
    MonoPopBack(dq);
  }
  MonoPushBack(dq, {idx, sample});
  while (!MonoEmpty(dq) && (idx + 1 - MonoFront(dq).first) > period) {
    MonoPopFront(dq);
  }
  ++idx;
  return MonoFront(dq).second;
}

double UpdateRollingMax(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample) {
  MonoInit(dq, period);
  while (!MonoEmpty(dq) && MonoBack(dq).second <= sample) {
    MonoPopBack(dq);
  }
  MonoPushBack(dq, {idx, sample});
  while (!MonoEmpty(dq) && (idx + 1 - MonoFront(dq).first) > period) {
    MonoPopFront(dq);
  }
  ++idx;
  return MonoFront(dq).second;
}

double UpdateRollingMinPrepared(MonoDequeState& dq, std::size_t& idx, double sample) {
  if (dq.cap == 0) {
    throw std::runtime_error("UpdateRollingMinPrepared called before state prewarm");
  }
  while (!MonoEmpty(dq) && MonoBack(dq).second >= sample) {
    MonoPopBack(dq);
  }
  MonoPushBack(dq, {idx, sample});
  const std::size_t period = dq.cap - 1;
  while (!MonoEmpty(dq) && (idx + 1 - MonoFront(dq).first) > period) {
    MonoPopFront(dq);
  }
  ++idx;
  return MonoFront(dq).second;
}

double UpdateRollingMaxPrepared(MonoDequeState& dq, std::size_t& idx, double sample) {
  if (dq.cap == 0) {
    throw std::runtime_error("UpdateRollingMaxPrepared called before state prewarm");
  }
  while (!MonoEmpty(dq) && MonoBack(dq).second <= sample) {
    MonoPopBack(dq);
  }
  MonoPushBack(dq, {idx, sample});
  const std::size_t period = dq.cap - 1;
  while (!MonoEmpty(dq) && (idx + 1 - MonoFront(dq).first) > period) {
    MonoPopFront(dq);
  }
  ++idx;
  return MonoFront(dq).second;
}

extern "C" double jit_rt_mid(const MarketState* state, std::int64_t symbol_id) {
  const std::size_t id = static_cast<std::size_t>(symbol_id);
  const InstrumentState& ins = state->instruments[id];
  return (ins.bid + ins.ask) * 0.5;
}

extern "C" SignalContext* jit_rt_symbol_ctx(MultiSymbolSignalContext* arena, std::uint32_t symbol_id) {
  return &arena->PerSymbol(symbol_id);
}

extern "C" double jit_rt_param(SignalContext* ctx, std::int64_t param_id) {
  if (param_id < 0 || static_cast<std::size_t>(param_id) >= ctx->num_params || ctx->params == nullptr) {
    throw std::runtime_error("Parameter id out of range or parameters not initialized");
  }
  return ctx->params[static_cast<std::size_t>(param_id)];
}

extern "C" double jit_rt_bid(const MarketState* state, std::int64_t symbol_id) {
  return state->instruments[static_cast<std::size_t>(symbol_id)].bid;
}

extern "C" double jit_rt_ask(const MarketState* state, std::int64_t symbol_id) {
  return state->instruments[static_cast<std::size_t>(symbol_id)].ask;
}

extern "C" double jit_rt_spread(const MarketState* state, std::int64_t symbol_id) {
  const InstrumentState& ins = state->instruments[static_cast<std::size_t>(symbol_id)];
  return ins.ask - ins.bid;
}

extern "C" double jit_rt_ema(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->ema_states.size());
  EMAState& st = ctx->ema_states[idx];
  if (!st.initialized) {
    st.value = x;
    st.period = period;
    st.alpha = 2.0 / (static_cast<double>(period) + 1.0);
    st.initialized = true;
    return st.value;
  }
  st.value = st.alpha * x + (1.0 - st.alpha) * st.value;
  return st.value;
}

extern "C" double jit_rt_ema_alpha(
    SignalContext* ctx, std::int64_t node_id, double x, double alpha, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->ema_states.size());
  EMAState& st = ctx->ema_states[idx];
  if (!st.initialized) {
    st.value = x;
    st.alpha = alpha;
    st.period = period;
    st.initialized = true;
    return st.value;
  }
  st.value = st.alpha * x + (1.0 - st.alpha) * st.value;
  return st.value;
}

extern "C" double jit_rt_sma(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->sma_states.size());
  RingStatsState& st = ctx->sma_states[idx];
  (void)period;
  RingStatsPushPrepared(st, x);
  if (!RingStatsFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return RingStatsMean(st);
}

extern "C" bool jit_rt_sma_prepare(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    std::int64_t period,
    const double** buffer_out,
    std::int64_t* size_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->sma_states.size());
  RingStatsState& st = ctx->sma_states[idx];
  (void)period;
  RingStatsPushPrepared(st, x);
  if (!RingStatsFull(st)) return false;
  *buffer_out = st.buffer.data();
  *size_out = static_cast<std::int64_t>(st.capacity);
  return true;
}

extern "C" double jit_rt_rolling_std(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->rolling_std_states.size());
  RingStatsState& st = ctx->rolling_std_states[idx];
  (void)period;
  RingStatsPushPrepared(st, x);
  if (!RingStatsFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return RingStatsStddevSample(st);
}

extern "C" double jit_rt_zscore(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->zscore_states.size());
  RingStatsState& st = ctx->zscore_states[idx];
  (void)period;
  RingStatsPushPrepared(st, x);
  if (!RingStatsFull(st)) return std::numeric_limits<double>::quiet_NaN();
  const double mean = RingStatsMean(st);
  const double stddev = RingStatsStddevSample(st);
  if (std::isnan(stddev) || std::fabs(stddev) < 1e-18) return std::numeric_limits<double>::quiet_NaN();
  return (x - mean) / stddev;
}

extern "C" double jit_rt_rolling_min(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t id = static_cast<std::size_t>(node_id);
  assert(id < ctx->rolling_min_deques.size());
  assert(id < ctx->rolling_min_indices.size());
  auto& dq = ctx->rolling_min_deques[id];
  std::size_t& idx = ctx->rolling_min_indices[id];
  (void)period;
  const double v = UpdateRollingMinPrepared(dq, idx, x);
  if (idx < static_cast<std::size_t>(period)) return std::numeric_limits<double>::quiet_NaN();
  return v;
}

extern "C" double jit_rt_rolling_max(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t id = static_cast<std::size_t>(node_id);
  assert(id < ctx->rolling_max_deques.size());
  assert(id < ctx->rolling_max_indices.size());
  auto& dq = ctx->rolling_max_deques[id];
  std::size_t& idx = ctx->rolling_max_indices[id];
  (void)period;
  const double v = UpdateRollingMaxPrepared(dq, idx, x);
  if (idx < static_cast<std::size_t>(period)) return std::numeric_limits<double>::quiet_NaN();
  return v;
}

extern "C" double jit_rt_vwap(
    const MarketState* state, SignalContext* ctx, std::int64_t node_id, std::int64_t symbol_id, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->vwap_states.size());
  const std::size_t instrument_id = static_cast<std::size_t>(symbol_id);
  const InstrumentState& ins = state->instruments[instrument_id];
  const double price = (ins.bid + ins.ask) * 0.5;
  const double volume = (ins.volume > 0.0) ? ins.volume : 1.0;
  VwapState& st = ctx->vwap_states[idx];
  (void)period;
  VwapPushPrepared(st, price, volume);
  if (!VwapFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return VwapValue(st);
}

extern "C" double jit_rt_lag(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->lag_states.size());
  LagState& st = ctx->lag_states[idx];
  const double lagged = LagValue(st);
  (void)period;
  LagPushPrepared(st, x);
  return lagged;
}

extern "C" double jit_rt_cross_above(SignalContext* ctx, std::int64_t node_id, double a, double b) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->cross_states.size());
  CrossState& st = ctx->cross_states[idx];
  if (!st.initialized) {
    st.prev_a = a;
    st.prev_b = b;
    st.initialized = true;
    return 0.0;
  }
  const bool crossed = (st.prev_a <= st.prev_b) && (a > b);
  st.prev_a = a;
  st.prev_b = b;
  return crossed ? 1.0 : 0.0;
}

extern "C" double jit_rt_cross_below(SignalContext* ctx, std::int64_t node_id, double a, double b) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->cross_states.size());
  CrossState& st = ctx->cross_states[idx];
  if (!st.initialized) {
    st.prev_a = a;
    st.prev_b = b;
    st.initialized = true;
    return 0.0;
  }
  const bool crossed = (st.prev_a >= st.prev_b) && (a < b);
  st.prev_a = a;
  st.prev_b = b;
  return crossed ? 1.0 : 0.0;
}

// P7 runtime entry points. Each one shares its state structure with the
// matching interpreter path so the parity test is a true bit-equality
// gate (modulo IEEE-754 reordering, which we don't do here -- the
// running-sum order is identical to the interpreter).
extern "C" double jit_rt_rolling_corr(
    SignalContext* ctx, std::int64_t node_id, double x, double y, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->rolling_corr_states.size());
  RollingPairState& st = ctx->rolling_corr_states[idx];
  RollingPairPush(st, static_cast<std::size_t>(period), x, y);
  return RollingPairCorrelation(st);
}

extern "C" double jit_rt_rolling_beta(
    SignalContext* ctx, std::int64_t node_id, double x, double y, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->rolling_beta_states.size());
  RollingPairState& st = ctx->rolling_beta_states[idx];
  RollingPairPush(st, static_cast<std::size_t>(period), x, y);
  return RollingPairBeta(st);
}

extern "C" double jit_rt_kalman1d(
    SignalContext* ctx, std::int64_t node_id, double x, double q, double r) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->kalman1d_states.size());
  Kalman1dState& st = ctx->kalman1d_states[idx];
  return Kalman1dStep(st, x, q, r);
}

extern "C" double jit_rt_ema_alpha_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double alpha,
    double alpha_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->ema_states.size());
  EMAState& primal = ctx->ema_states[idx];
  EmaSensitivityState& grad = ctx->ema_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  if (!primal.initialized) {
    primal.value = x;
    primal.alpha = alpha;
    primal.period = period;
    primal.initialized = true;
    grad.value = x_grad;
    *grad_out = grad.value;
    return primal.value;
  }
  const double prev = primal.value;
  const double prev_grad = grad.value;
  primal.alpha = alpha;
  primal.period = period;
  primal.value = alpha * x + (1.0 - alpha) * primal.value;
  grad.value = alpha * x_grad + (1.0 - alpha) * prev_grad + alpha_grad * (x - prev);
  *grad_out = grad.value;
  return primal.value;
}

extern "C" double jit_rt_sma_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->sma_states.size());
  RingStatsState& primal = ctx->sma_states[idx];
  RingStatsState& grad = ctx->sma_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  (void)period;
  RingStatsPushPrepared(primal, x);
  RingStatsPush(grad, static_cast<std::size_t>(period), x_grad);
  if (!RingStatsFull(primal)) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  *grad_out = RingStatsMean(grad);
  return RingStatsMean(primal);
}

extern "C" double jit_rt_lag_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->lag_states.size());
  LagState& primal = ctx->lag_states[idx];
  LagState& grad = ctx->lag_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  const double lagged = LagValue(primal);
  *grad_out = LagValue(grad);
  (void)period;
  LagPushPrepared(primal, x);
  LagPush(grad, static_cast<std::size_t>(period), x_grad);
  return lagged;
}

extern "C" double jit_rt_rolling_std_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  return RollingStdOrZscoreGradStep(
      *ctx, idx, x, x_grad, static_cast<std::size_t>(period), grad_param, false, grad_out);
}

extern "C" double jit_rt_zscore_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  return RollingStdOrZscoreGradStep(
      *ctx, idx, x, x_grad, static_cast<std::size_t>(period), grad_param, true, grad_out);
}

extern "C" double jit_rt_rolling_corr_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double y,
    double y_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->rolling_corr_states.size());
  RollingPairState& primal = ctx->rolling_corr_states[idx];
  RollingPairSensitivityState& grad =
      ctx->rolling_corr_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  const std::size_t p = static_cast<std::size_t>(period);
  if (primal.capacity != p) {
    primal.x_buf.assign(p, 0.0);
    primal.y_buf.assign(p, 0.0);
    primal.capacity = p;
    primal.head = 0;
    primal.count = 0;
    primal.sum_x = primal.sum_y = primal.sum_xy = primal.sum_xx = primal.sum_yy = 0.0L;
  }
  if (grad.capacity != p) {
    grad.x_buf.assign(p, 0.0L);
    grad.y_buf.assign(p, 0.0L);
    grad.capacity = p;
    grad.head = 0;
    grad.count = 0;
    grad.sum_x = grad.sum_y = grad.sum_xy = grad.sum_xx = grad.sum_yy = 0.0L;
  }
  if (primal.count == primal.capacity) {
    const long double old_xp = grad.x_buf[primal.head];
    const long double old_yp = grad.y_buf[primal.head];
    const double old_x = primal.x_buf[primal.head];
    const double old_y = primal.y_buf[primal.head];
    primal.sum_x -= old_x;
    primal.sum_y -= old_y;
    primal.sum_xy -= static_cast<long double>(old_x) * static_cast<long double>(old_y);
    primal.sum_xx -= static_cast<long double>(old_x) * static_cast<long double>(old_x);
    primal.sum_yy -= static_cast<long double>(old_y) * static_cast<long double>(old_y);
    grad.sum_x -= old_xp;
    grad.sum_y -= old_yp;
    grad.sum_xy -= old_xp * old_y + old_x * old_yp;
    grad.sum_xx -= 2.0L * static_cast<long double>(old_x) * old_xp;
    grad.sum_yy -= 2.0L * static_cast<long double>(old_y) * old_yp;
  } else {
    ++primal.count;
    ++grad.count;
  }
  primal.x_buf[primal.head] = x;
  primal.y_buf[primal.head] = y;
  primal.sum_x += x;
  primal.sum_y += y;
  primal.sum_xy += static_cast<long double>(x) * static_cast<long double>(y);
  primal.sum_xx += static_cast<long double>(x) * static_cast<long double>(x);
  primal.sum_yy += static_cast<long double>(y) * static_cast<long double>(y);
  grad.x_buf[primal.head] = static_cast<long double>(x_grad);
  grad.y_buf[primal.head] = static_cast<long double>(y_grad);
  grad.sum_x += x_grad;
  grad.sum_y += y_grad;
  grad.sum_xy += x_grad * y + x * y_grad;
  grad.sum_xx += 2.0L * x * x_grad;
  grad.sum_yy += 2.0L * y * y_grad;
  primal.head = (primal.head + 1) % primal.capacity;
  grad.head = primal.head;
  if (!RollingPairFull(primal)) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const long double n = static_cast<long double>(primal.count);
  const long double cov = primal.sum_xy - (primal.sum_x * primal.sum_y) / n;
  const long double cov_prime =
      grad.sum_xy - (grad.sum_x * primal.sum_y + primal.sum_x * grad.sum_y) / n;
  const long double var_x = primal.sum_xx - (primal.sum_x * primal.sum_x) / n;
  const long double var_x_prime = grad.sum_xx - 2.0L * primal.sum_x * grad.sum_x / n;
  const long double var_y = primal.sum_yy - (primal.sum_y * primal.sum_y) / n;
  const long double var_y_prime = grad.sum_yy - 2.0L * primal.sum_y * grad.sum_y / n;
  const double corr = RollingPairCorrelation(primal);
  const long double denom = std::sqrt(static_cast<double>(var_x * var_y));
  const long double denom_prime =
      (var_x_prime * var_y + var_x * var_y_prime) / (2.0L * denom);
  const long double corr_prime = cov_prime / denom - cov * denom_prime / (denom * denom);
  *grad_out = static_cast<double>(corr_prime);
  return corr;
}

extern "C" double jit_rt_rolling_beta_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double y,
    double y_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->rolling_beta_states.size());
  RollingPairState& primal = ctx->rolling_beta_states[idx];
  RollingPairSensitivityState& grad =
      ctx->rolling_beta_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  const std::size_t p = static_cast<std::size_t>(period);
  if (primal.capacity != p) {
    primal.x_buf.assign(p, 0.0);
    primal.y_buf.assign(p, 0.0);
    primal.capacity = p;
    primal.head = 0;
    primal.count = 0;
    primal.sum_x = primal.sum_y = primal.sum_xy = primal.sum_xx = primal.sum_yy = 0.0L;
  }
  if (grad.capacity != p) {
    grad.x_buf.assign(p, 0.0L);
    grad.y_buf.assign(p, 0.0L);
    grad.capacity = p;
    grad.head = 0;
    grad.count = 0;
    grad.sum_x = grad.sum_y = grad.sum_xy = grad.sum_xx = grad.sum_yy = 0.0L;
  }
  if (primal.count == primal.capacity) {
    const long double old_xp = grad.x_buf[primal.head];
    const long double old_yp = grad.y_buf[primal.head];
    const double old_x = primal.x_buf[primal.head];
    const double old_y = primal.y_buf[primal.head];
    primal.sum_x -= old_x;
    primal.sum_y -= old_y;
    primal.sum_xy -= static_cast<long double>(old_x) * static_cast<long double>(old_y);
    primal.sum_xx -= static_cast<long double>(old_x) * static_cast<long double>(old_x);
    primal.sum_yy -= static_cast<long double>(old_y) * static_cast<long double>(old_y);
    grad.sum_x -= old_xp;
    grad.sum_y -= old_yp;
    grad.sum_xy -= old_xp * old_y + old_x * old_yp;
    grad.sum_xx -= 2.0L * static_cast<long double>(old_x) * old_xp;
    grad.sum_yy -= 2.0L * static_cast<long double>(old_y) * old_yp;
  } else {
    ++primal.count;
    ++grad.count;
  }
  primal.x_buf[primal.head] = x;
  primal.y_buf[primal.head] = y;
  primal.sum_x += x;
  primal.sum_y += y;
  primal.sum_xy += static_cast<long double>(x) * static_cast<long double>(y);
  primal.sum_xx += static_cast<long double>(x) * static_cast<long double>(x);
  primal.sum_yy += static_cast<long double>(y) * static_cast<long double>(y);
  grad.x_buf[primal.head] = static_cast<long double>(x_grad);
  grad.y_buf[primal.head] = static_cast<long double>(y_grad);
  grad.sum_x += x_grad;
  grad.sum_y += y_grad;
  grad.sum_xy += x_grad * y + x * y_grad;
  grad.sum_xx += 2.0L * x * x_grad;
  grad.sum_yy += 2.0L * y * y_grad;
  primal.head = (primal.head + 1) % primal.capacity;
  grad.head = primal.head;
  if (!RollingPairFull(primal)) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const long double n = static_cast<long double>(primal.count);
  const long double cov = primal.sum_xy - (primal.sum_x * primal.sum_y) / n;
  const long double cov_prime =
      grad.sum_xy - (grad.sum_x * primal.sum_y + primal.sum_x * grad.sum_y) / n;
  const long double var_x = primal.sum_xx - (primal.sum_x * primal.sum_x) / n;
  const long double var_x_prime = grad.sum_xx - 2.0L * primal.sum_x * grad.sum_x / n;
  *grad_out = static_cast<double>((cov_prime * var_x - cov * var_x_prime) / (var_x * var_x));
  return RollingPairBeta(primal);
}

extern "C" double jit_rt_kalman1d_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double q,
    double q_grad,
    double r,
    double r_grad,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->kalman1d_states.size());
  Kalman1dState& primal = ctx->kalman1d_states[idx];
  Kalman1dSensitivityState& grad =
      ctx->kalman1d_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  if (!primal.initialized) {
    primal.x_hat = x;
    primal.p = r;
    primal.q = q;
    primal.r = r;
    primal.initialized = true;
    grad.x_hat = x_grad;
    grad.p = r_grad;
    *grad_out = grad.x_hat;
    return primal.x_hat;
  }
  const double p_pred = primal.p + q;
  const double p_pred_grad = grad.p + q_grad;
  const double denom = p_pred + r;
  const double denom_grad = p_pred_grad + r_grad;
  if (denom <= 0.0) {
    *grad_out = grad.x_hat;
    return primal.x_hat;
  }
  const double k = p_pred / denom;
  const double k_grad = (p_pred_grad * denom - p_pred * denom_grad) / (denom * denom);
  const double innov = x - primal.x_hat;
  const double innov_grad = x_grad - grad.x_hat;
  const double old_x_hat_grad = grad.x_hat;
  primal.x_hat = primal.x_hat + k * innov;
  grad.x_hat = old_x_hat_grad + k_grad * innov + k * innov_grad;
  const double p_new = (1.0 - k) * p_pred;
  const double p_new_grad = -k_grad * p_pred + (1.0 - k) * p_pred_grad;
  if (p_new < 0.0) {
    primal.p = 0.0;
    grad.p = 0.0;
  } else {
    primal.p = p_new;
    grad.p = p_new_grad;
  }
  primal.q = q;
  primal.r = r;
  *grad_out = grad.x_hat;
  return primal.x_hat;
}

extern "C" double jit_rt_rolling_min_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->rolling_min_deques.size());
  auto& dq = ctx->rolling_min_deques[idx];
  std::size_t& tick_idx = ctx->rolling_min_indices[idx];
  LagState& grad_ring = ctx->rolling_min_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  const std::size_t p = static_cast<std::size_t>(period);
  const std::size_t before_idx = tick_idx;
  const double out = UpdateRollingMinPrepared(dq, tick_idx, x);
  if (grad_ring.capacity != p) {
    grad_ring.buffer.assign(p, 0.0);
    grad_ring.capacity = p;
    grad_ring.head = 0;
    grad_ring.count = 0;
  }
  if (grad_ring.capacity > 0) {
    if (grad_ring.count < grad_ring.capacity) ++grad_ring.count;
    grad_ring.buffer[before_idx % grad_ring.capacity] = x_grad;
    grad_ring.head = tick_idx % grad_ring.capacity;
  }
  if (tick_idx < p) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const std::size_t active_tick = dq.buf[dq.head].first;
  *grad_out = grad_ring.buffer[active_tick % p];
  return out;
}

extern "C" double jit_rt_rolling_max_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  const std::size_t grad_param = CheckedGradientParam(*ctx, param_id);
  assert(idx < ctx->rolling_max_deques.size());
  auto& dq = ctx->rolling_max_deques[idx];
  std::size_t& tick_idx = ctx->rolling_max_indices[idx];
  LagState& grad_ring = ctx->rolling_max_sensitivity_states[GradientSlot(*ctx, idx, grad_param)];
  const std::size_t p = static_cast<std::size_t>(period);
  const std::size_t before_idx = tick_idx;
  const double out = UpdateRollingMaxPrepared(dq, tick_idx, x);
  if (grad_ring.capacity != p) {
    grad_ring.buffer.assign(p, 0.0);
    grad_ring.capacity = p;
    grad_ring.head = 0;
    grad_ring.count = 0;
  }
  if (grad_ring.capacity > 0) {
    if (grad_ring.count < grad_ring.capacity) ++grad_ring.count;
    grad_ring.buffer[before_idx % grad_ring.capacity] = x_grad;
    grad_ring.head = tick_idx % grad_ring.capacity;
  }
  if (tick_idx < p) {
    *grad_out = std::numeric_limits<double>::quiet_NaN();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const std::size_t active_tick = dq.buf[dq.head].first;
  *grad_out = grad_ring.buffer[active_tick % p];
  return out;
}

extern "C" double jit_rt_cross_above_grad(
    SignalContext* ctx, std::int64_t node_id, double a, double b, std::int64_t param_id, double* grad_out) {
  (void)param_id;
  *grad_out = 0.0;
  return jit_rt_cross_above(ctx, node_id, a, b);
}

extern "C" double jit_rt_cross_below_grad(
    SignalContext* ctx, std::int64_t node_id, double a, double b, std::int64_t param_id, double* grad_out) {
  (void)param_id;
  *grad_out = 0.0;
  return jit_rt_cross_below(ctx, node_id, a, b);
}

// P0 lowered-state base accessors. Called once per JIT function (the IR
// hoists the result), so the per-op cost is one ptr-arith.
extern "C" SmaStateLowered* jit_rt_sma_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.sma;
}
extern "C" EmaStateLowered* jit_rt_ema_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.ema;
}
extern "C" LagStateLowered* jit_rt_lag_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.lag;
}
extern "C" RollingStdStateLowered* jit_rt_rolling_std_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.rolling_std;
}
extern "C" RollingStdStateLowered* jit_rt_zscore_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.zscore;
}
extern "C" RollingMinMaxStateLowered* jit_rt_rolling_min_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.rolling_min;
}
extern "C" RollingMinMaxStateLowered* jit_rt_rolling_max_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.rolling_max;
}
extern "C" CrossStateLowered* jit_rt_cross_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.cross;
}
extern "C" Kalman1dStateLowered* jit_rt_kalman1d_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.kalman1d;
}
extern "C" VwapStateLowered* jit_rt_vwap_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.vwap;
}
extern "C" RollingPairStateLowered* jit_rt_rolling_corr_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.rolling_corr;
}
extern "C" RollingPairStateLowered* jit_rt_rolling_beta_lowered_base(SignalContext* ctx) {
  return ctx->lowered_bases.rolling_beta;
}

}  // namespace jitse
