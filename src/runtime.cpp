#include "runtime.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>

#include "ast.h"

namespace jitse {

namespace {

void MonoInit(MonoDequeState& st, std::size_t period) {
  const std::size_t needed_cap = period + 1;
  if (st.cap == needed_cap) return;
  st.buf.assign(needed_cap, {0, 0.0});
  st.head = 0;
  st.count = 0;
  st.cap = needed_cap;
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
    state.sumsq = 0.0L;
  }

  if (state.count == state.capacity) {
    const double old = state.buffer[state.head];
    state.sum -= old;
    state.sumsq -= static_cast<long double>(old) * static_cast<long double>(old);
  } else {
    ++state.count;
  }

  state.buffer[state.head] = sample;
  state.sum += sample;
  state.sumsq += static_cast<long double>(sample) * static_cast<long double>(sample);
  state.head = (state.head + 1) % state.capacity;
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
  const long double n = static_cast<long double>(state.count);
  const long double mean = state.sum / n;
  long double var = (state.sumsq - n * mean * mean) / (n - 1.0L);
  if (var < 0.0L && var > -1e-18L) {
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
    state.total_samples = 0;
  }
  state.buffer[state.head] = sample;
  state.head = (state.head + 1) % state.capacity;
  if (state.count < state.capacity) ++state.count;
  ++state.total_samples;
}

double LagValue(const LagState& state) {
  if (state.capacity == 0 || state.count < state.capacity) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return state.buffer[state.head];
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
}

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
        EnsureNodeCapacity(ctx, static_cast<std::size_t>(fn->node_id));
      }
      for (const auto& a : fn->args) {
        walk(*a);
      }
      return;
    }
  };
  walk(*signal.body);
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

extern "C" double jit_rt_mid(const MarketState* state, std::int64_t symbol_id) {
  const std::size_t id = static_cast<std::size_t>(symbol_id);
  const InstrumentState& ins = state->instruments[id];
  return (ins.bid + ins.ask) * 0.5;
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
    st.initialized = true;
    return st.value;
  }
  const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
  st.value = alpha * x + (1.0 - alpha) * st.value;
  return st.value;
}

extern "C" double jit_rt_sma(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->sma_states.size());
  RingStatsState& st = ctx->sma_states[idx];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
  if (!RingStatsFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return RingStatsMean(st);
}

extern "C" double jit_rt_rolling_std(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->rolling_std_states.size());
  RingStatsState& st = ctx->rolling_std_states[idx];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
  if (!RingStatsFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return RingStatsStddevSample(st);
}

extern "C" double jit_rt_zscore(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->zscore_states.size());
  RingStatsState& st = ctx->zscore_states[idx];
  RingStatsPush(st, static_cast<std::size_t>(period), x);
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
  const double v = UpdateRollingMin(dq, idx, static_cast<std::size_t>(period), x);
  if (idx < static_cast<std::size_t>(period)) return std::numeric_limits<double>::quiet_NaN();
  return v;
}

extern "C" double jit_rt_rolling_max(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t id = static_cast<std::size_t>(node_id);
  assert(id < ctx->rolling_max_deques.size());
  assert(id < ctx->rolling_max_indices.size());
  auto& dq = ctx->rolling_max_deques[id];
  std::size_t& idx = ctx->rolling_max_indices[id];
  const double v = UpdateRollingMax(dq, idx, static_cast<std::size_t>(period), x);
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
  VwapPush(st, static_cast<std::size_t>(period), price, volume);
  if (!VwapFull(st)) return std::numeric_limits<double>::quiet_NaN();
  return VwapValue(st);
}

extern "C" double jit_rt_lag(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period) {
  const std::size_t idx = static_cast<std::size_t>(node_id);
  assert(idx < ctx->lag_states.size());
  LagState& st = ctx->lag_states[idx];
  const double lagged = LagValue(st);
  LagPush(st, static_cast<std::size_t>(period), x);
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

}  // namespace jitse
