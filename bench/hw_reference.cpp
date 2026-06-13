#include "hw_reference.h"

#include <cmath>
#include <limits>

namespace jitse {
namespace hw {
namespace {

inline double Mid(const MarketState& m, std::size_t sym) {
  const auto& ins = m.instruments[sym];
  return (ins.bid + ins.ask) * 0.5;
}

inline double EmaStep(EmaSlot& st, double x, int period) {
  if (!st.initialized) {
    st.alpha = 2.0 / (static_cast<double>(period) + 1.0);
    st.value = x;
    st.initialized = true;
    return st.value;
  }
  st.value = st.alpha * x + (1.0 - st.alpha) * st.value;
  return st.value;
}

inline double ZscoreStep(RingStatsState& ring, double x, std::size_t period) {
  if (ring.capacity != period) {
    ring.buffer.assign(period, 0.0);
    ring.capacity = period;
    ring.head = 0;
    ring.count = 0;
    ring.sum = 0.0L;
    ring.mean = 0.0L;
    ring.m2 = 0.0L;
    ring.slides_since_refresh = 0;
  }
  RingStatsPushPrepared(ring, x);
  if (!RingStatsFull(ring)) return std::numeric_limits<double>::quiet_NaN();
  const double mean = RingStatsMean(ring);
  const double stddev = RingStatsStddevSample(ring);
  if (std::isnan(stddev) || std::fabs(stddev) < 1e-18) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return (x - mean) / stddev;
}

}  // namespace

Profile ProfileForBenchmark(bool all_signals_mode, const std::string& signal_name) {
  if (all_signals_mode) return Profile::kFilteredMomentum;
  if (signal_name == "spread") return Profile::kSpread;
  if (signal_name == "momentum") return Profile::kMomentum;
  if (signal_name == "spread_z") return Profile::kSpreadZ;
  if (signal_name == "z") return Profile::kZ;
  if (signal_name == "dev") return Profile::kDev;
  return Profile::kNone;
}

void ResetState(State& st, Profile profile) {
  st = State{};
  if (profile == Profile::kSpreadZ || profile == Profile::kDev || profile == Profile::kFilteredMomentum) {
    st.rstd_aapl.capacity = 60;
    st.rstd_aapl.buffer.assign(60, 0.0);
  }
  if (profile == Profile::kDev || profile == Profile::kFilteredMomentum) {
    st.rstd_aapl_dev.capacity = 30;
    st.rstd_aapl_dev.buffer.assign(30, 0.0);
    st.vwap_aapl.capacity = 30;
    st.vwap_aapl.price_buf.assign(30, 0.0);
    st.vwap_aapl.vol_buf.assign(30, 0.0);
  }
  if (profile == Profile::kZ) {
    st.zscore_ring.capacity = 30;
    st.zscore_ring.buffer.assign(30, 0.0);
  }
}

double Tick(State& st, Profile profile, const MarketState& market) {
  switch (profile) {
    case Profile::kSpread: {
      const double mid0 = Mid(market, 0);
      const double mid1 = Mid(market, 1);
      return mid0 - mid1;
    }
    case Profile::kMomentum: {
      const double m = Mid(market, 0);
      const double fast = EmaStep(st.ema_short, m, 10);
      const double slow = EmaStep(st.ema_long, m, 60);
      return fast - slow;
    }
    case Profile::kSpreadZ: {
      const double a = Mid(market, 0);
      const double b = Mid(market, 1);
      const double ea = EmaStep(st.ema_aapl, a, 30);
      const double eb = EmaStep(st.ema_msft, b, 30);
      RingStatsPushPrepared(st.rstd_aapl, a);
      if (!RingStatsFull(st.rstd_aapl)) return std::numeric_limits<double>::quiet_NaN();
      const double vol = RingStatsStddevSample(st.rstd_aapl);
      if (vol <= 0.0 || std::isnan(vol)) return std::numeric_limits<double>::quiet_NaN();
      return (ea - eb) / vol;
    }
    case Profile::kZ: {
      const double m = Mid(market, 0);
      const double fast = EmaStep(st.ema_fast, m, 10);
      const double slow = EmaStep(st.ema_slow, m, 30);
      return ZscoreStep(st.zscore_ring, fast - slow, 30);
    }
    case Profile::kDev: {
      const double m = Mid(market, 0);
      const double vol = (market.instruments[0].volume > 0.0) ? market.instruments[0].volume : 1.0;
      VwapPushPrepared(st.vwap_aapl, m, vol);
      RingStatsPushPrepared(st.rstd_aapl_dev, m);
      if (!VwapFull(st.vwap_aapl) || !RingStatsFull(st.rstd_aapl_dev)) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      const double vwap = VwapValue(st.vwap_aapl);
      const double stddev = RingStatsStddevSample(st.rstd_aapl_dev);
      if (stddev <= 0.0 || std::isnan(stddev)) return std::numeric_limits<double>::quiet_NaN();
      return (m - vwap) / stddev;
    }
    case Profile::kFilteredMomentum: {
      const double m = Mid(market, 0);
      const double short_ma = EmaStep(st.ema_short, m, 10);
      const double long_ma = EmaStep(st.ema_long, m, 60);
      RingStatsPushPrepared(st.rstd_aapl_dev, m);
      if (!RingStatsFull(st.rstd_aapl_dev)) return std::numeric_limits<double>::quiet_NaN();
      const double vol = RingStatsStddevSample(st.rstd_aapl_dev);
      const double raw = short_ma - long_ma;
      if (short_ma > long_ma && vol > 0.0) return raw / vol;
      return 0.0;
    }
    case Profile::kNone:
    default:
      return std::numeric_limits<double>::quiet_NaN();
  }
}

}  // namespace hw
}  // namespace jitse
