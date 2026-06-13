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

// Rolling-window stats state used by sma / rolling_std / zscore.
//
// Two parallel running aggregates are maintained on every push, both in
// long-double precision so the catastrophic-cancellation threshold is
// pushed out by ~11 bits of mantissa relative to a double-precision
// accumulator:
//
//   * `sum`  -- plain running sum (O(1) updates; consumed by
//               RingStatsMean). Kept for backward compatibility.
//   * `mean` -- Welford running mean. Updated on add via
//               `mean += (x - mean) / count` and on remove via
//               `mean -= (x_old - mean) / (count - 1)` (West 1979 rolling
//               variant). Reading the mean directly avoids a divide on
//               every observation.
//   * `m2`   -- Welford sum of squared deviations from the *running*
//               mean. Updated incrementally so the sample stddev is
//               recoverable in O(1) as `sqrt(m2 / (count - 1))`. This is
//               the rolling Welford recurrence (add-then-remove on a
//               full window) and is what makes RingStatsStddevSample an
//               O(1) operation instead of an O(period) two-pass loop.
//
// The two-pass `sum_of_squared_deviations` formula is preserved as
// `RingStatsStddevSampleTwoPassReference` and is used by
// `welford_stddev_parity_test` as the numerical-accuracy oracle. The
// parity test gates that the incremental and two-pass paths agree to
// within `1e-10` relative error on benign inputs and within `1e-6` even
// in catastrophic-cancellation regimes (large mean, tiny variance).
//
// Catastrophic-cancellation robustness story: pure rolling Welford
// accumulates roundoff in `m2` because every slide does a remove step
// `m2 -= delta * (old - mean)` whose precision is bounded by the
// representable resolution of `(old - mean)` — when both terms are on
// the order of 1e7 with a true difference of 1e-6, long-double loses
// ~13 digits there. To bound the resulting drift to one window's worth
// of slides (instead of growing with stream length), we recompute
// `mean` and `m2` from the buffer once every `capacity` slide
// operations. That is O(capacity) work per O(capacity) slides, i.e.
// O(1) amortized, while pinning the rolling Welford error to
// "one window of long-double roundoff" regardless of how long the
// stream runs. The `slides_since_refresh` counter tracks slide-only
// updates (window-fill adds don't count, because they don't run the
// remove-step that drifts).
struct RingStatsState {
  std::vector<double> buffer;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double sum = 0.0L;
  long double mean = 0.0L;
  long double m2 = 0.0L;
  std::size_t slides_since_refresh = 0;
};

struct EMAState {
  double value = 0.0;
  double alpha = 0.0;
  std::int64_t period = 0;
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

// POD layout for P0 lowered rolling_min / rolling_max IR (must match jit_compiler.cpp).
struct MonoDequeEntryLowered {
  std::int64_t tick_index;
  double value;
};

struct RollingMinMaxStateLowered {
  MonoDequeEntryLowered* buf;
  std::int64_t head;
  std::int64_t count;
  std::int64_t cap;
  std::int64_t idx;
};

struct LagState {
  std::vector<double> buffer;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
};

struct CrossState {
  double prev_a = 0.0;
  double prev_b = 0.0;
  bool initialized = false;
};

// P7: state for paired-series rolling statistics (`rolling_corr`,
// `rolling_beta`). Both ops share the same accumulator: a fixed-length
// ring buffer over the most recent `capacity` (x, y) pairs plus running
// sums of x, y, x*y, x*x, y*y. The recurrences are the textbook ones --
// when the buffer is full we subtract the expiring pair's contributions
// before adding the new pair's, which lets each update run in O(1)
// regardless of `capacity`.
//
// Numerical stability story: the running-sum approach is the same one
// the existing RingStatsState uses for SMA and rolling_std, with the
// same caveat -- catastrophic cancellation is possible when the window
// is long, the inputs have a near-constant offset, and the variance is
// tiny. For typical signal-engine workloads (window <= ~10k, prices in
// the [1, 10^5] range, returns near-zero-mean) the relative error of
// the resulting correlation is below 1e-12. For workloads where this
// is not acceptable, the parity oracle test will catch divergence
// against a Welford-style streaming reference; we explicitly keep the
// running-sum form because it interleaves with the rest of the engine.
//
// Long-double accumulators are used (matching RingStatsState) so the
// catastrophic-cancellation threshold is pushed out by another 11 bits
// of mantissa relative to a double-precision running sum.
struct RollingPairState {
  std::vector<double> x_buf;
  std::vector<double> y_buf;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double sum_x = 0.0L;
  long double sum_y = 0.0L;
  long double sum_xy = 0.0L;
  long double sum_xx = 0.0L;
  long double sum_yy = 0.0L;
};

// P7: state for the 1-D Kalman filter (`kalman1d`). The textbook scalar
// Kalman filter has two parameters: process noise `q` and measurement
// noise `r`. The state carries the posterior estimate `x_hat` and the
// posterior error variance `p`. We use the standard predict-update
// recurrence; on the first sample we initialize x_hat to the
// measurement and p to r (so the first posterior matches the
// measurement exactly, then variance shrinks as more measurements
// arrive).
//
// Numerical stability: the textbook 1-D scalar form does not suffer the
// covariance-non-positive-definite issues of the multivariate Kalman
// (Joseph form / square-root filters) because all quantities are
// scalars >= 0. We add a small clamp so p never goes negative due to
// catastrophic cancellation (`p = max(0, p)`).
struct Kalman1dState {
  double x_hat = 0.0;
  double p = 0.0;
  double q = 0.0;
  double r = 0.0;
  bool initialized = false;
};

struct EmaSensitivityState {
  double value = 0.0;
};

struct RollingStdSensitivityState {
  std::vector<long double> buffer;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double mean = 0.0L;
  long double m2 = 0.0L;
  std::size_t slides_since_refresh = 0;
};

struct RollingPairSensitivityState {
  std::vector<long double> x_buf;
  std::vector<long double> y_buf;
  std::size_t capacity = 0;
  std::size_t head = 0;
  std::size_t count = 0;
  long double sum_x = 0.0L;
  long double sum_y = 0.0L;
  long double sum_xy = 0.0L;
  long double sum_xx = 0.0L;
  long double sum_yy = 0.0L;
};

struct Kalman1dSensitivityState {
  double x_hat = 0.0;
  double p = 0.0;
};

// ----------------------------------------------------------------------------
// Lowered-state structs (P0 IR lowering for stateful operators).
//
// These structs have a deliberately fixed, layout-stable POD shape so the JIT
// can compute field addresses directly in IR without going through extern "C"
// helpers. The C++ runtime never reads or writes these structs except during
// prewarm (which sets up `buffer`/`capacity`) and via the lowered IR path.
//
// The classic `EMAState`/`RingStatsState`/`LagState` above remain the source
// of truth for the interpreter and the non-lowered JIT path; the lowered
// state slots are parallel arrays populated by the same prewarm.
//
// Field order is locked by static_asserts in runtime.cpp. Do not reorder or
// the codegen offsets in jit_compiler.cpp will silently produce wrong code.
// ----------------------------------------------------------------------------

struct SmaStateLowered {
  double* buffer;       // offset 0, size 8: raw pointer to ring buffer of size `capacity`
  double sum;           // offset 8, size 8: running sum (double, not long double, for stable IR layout)
  std::int64_t head;    // offset 16
  std::int64_t count;   // offset 24
  std::int64_t capacity;// offset 32 (== compile-time period; redundant but kept for ABI safety)
};

struct EmaStateLowered {
  double value;             // offset 0
  std::int64_t initialized; // offset 8: 0 or 1 (i64 not bool to avoid padding ambiguity)
};

struct LagStateLowered {
  double* buffer;       // offset 0
  std::int64_t head;    // offset 8
  std::int64_t count;   // offset 16
  std::int64_t capacity;// offset 24
};

struct RollingStdStateLowered {
  double* buffer;               // offset 0
  std::int64_t capacity;        // offset 8
  std::int64_t head;            // offset 16
  std::int64_t count;           // offset 24
  long double sum;              // offset 32
  long double mean;             // offset 48 (used as scratch in lowered IR)
  long double m2;               // offset 64 (used as running sumsq in lowered IR)
  std::int64_t slides_since_refresh; // offset 80
};

struct CrossStateLowered {
  double prev_a;                // offset 0
  double prev_b;                // offset 8
  std::int64_t initialized;     // offset 16
};

struct Kalman1dStateLowered {
  double x_hat;                 // offset 0
  double p;                     // offset 8
  std::int64_t initialized;     // offset 16
};

struct VwapStateLowered {
  double* price_buf;            // offset 0
  double* vol_buf;              // offset 8
  std::int64_t head;            // offset 16
  std::int64_t count;           // offset 24
  std::int64_t capacity;        // offset 32
  double sum_pv;                // offset 40
  double sum_vol;               // offset 48
};

struct RollingPairStateLowered {
  double* x_buf;                // offset 0
  double* y_buf;                // offset 8
  std::int64_t head;            // offset 16
  std::int64_t count;           // offset 24
  std::int64_t capacity;        // offset 32
  std::int64_t _pad;            // offset 40 (align long double to 16 bytes)
  long double sum_x;            // offset 48
  long double sum_y;            // offset 64
  long double sum_xy;           // offset 80
  long double sum_xx;           // offset 96
  long double sum_yy;           // offset 112
};

// POD cache of lowered-state array bases. Must be the first member of
// SignalContext so JIT IR can load bases via offset-0 GEP from ctx*.
struct LoweredStateBases {
  SmaStateLowered* sma = nullptr;
  EmaStateLowered* ema = nullptr;
  LagStateLowered* lag = nullptr;
  RollingStdStateLowered* rolling_std = nullptr;
  RollingStdStateLowered* zscore = nullptr;
  RollingMinMaxStateLowered* rolling_min = nullptr;
  RollingMinMaxStateLowered* rolling_max = nullptr;
  CrossStateLowered* cross = nullptr;
  Kalman1dStateLowered* kalman1d = nullptr;
  VwapStateLowered* vwap = nullptr;
  RollingPairStateLowered* rolling_corr = nullptr;
  RollingPairStateLowered* rolling_beta = nullptr;
};

struct SignalContext {
  LoweredStateBases lowered_bases;
  std::vector<double> owned_params;
  const double* params = nullptr;
  std::size_t num_params = 0;
  std::size_t gradient_param_count = 0;
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

  // P7 paired-series stats: rolling_corr and rolling_beta share the same
  // state shape, so the engine uses two parallel arrays (one per op).
  // Mixing them would require an extra "which op?" discriminator on every
  // access; keeping them split costs one extra resize per node_id.
  std::vector<RollingPairState> rolling_corr_states;
  std::vector<RollingPairState> rolling_beta_states;

  // P7 Kalman filter state, one per node_id with a kalman1d call.
  std::vector<Kalman1dState> kalman1d_states;

  // Autodiff Phase 2: per-(node_id,param_id) sensitivity state. Flattened as
  // [node_id * gradient_param_count + param_id] so it reuses the existing
  // node_id discipline without any hash lookups on the tick path.
  std::vector<EmaSensitivityState> ema_sensitivity_states;
  std::vector<RingStatsState> sma_sensitivity_states;
  std::vector<LagState> lag_sensitivity_states;
  std::vector<RollingStdSensitivityState> rolling_std_sensitivity_states;
  std::vector<RollingStdSensitivityState> zscore_sensitivity_states;
  std::vector<RollingPairSensitivityState> rolling_corr_sensitivity_states;
  std::vector<RollingPairSensitivityState> rolling_beta_sensitivity_states;
  std::vector<Kalman1dSensitivityState> kalman1d_sensitivity_states;
  std::vector<LagState> rolling_min_sensitivity_states;
  std::vector<LagState> rolling_max_sensitivity_states;

  // P0 lowered-state arrays. Sized in lockstep with the runtime-call arrays.
  // Backing storage for the ring buffers lives in *_lowered_buffers so the
  // POD structs can hold a stable raw pointer the JIT IR indexes by node_id.
  std::vector<SmaStateLowered> sma_lowered;
  std::vector<std::vector<double>> sma_lowered_buffers;
  std::vector<EmaStateLowered> ema_lowered;
  std::vector<LagStateLowered> lag_lowered;
  std::vector<std::vector<double>> lag_lowered_buffers;
  std::vector<RollingStdStateLowered> rolling_std_lowered;
  std::vector<std::vector<double>> rolling_std_lowered_buffers;
  std::vector<RollingStdStateLowered> zscore_lowered;
  std::vector<std::vector<double>> zscore_lowered_buffers;
  std::vector<RollingMinMaxStateLowered> rolling_min_lowered;
  std::vector<std::vector<MonoDequeEntryLowered>> rolling_min_lowered_buffers;
  std::vector<RollingMinMaxStateLowered> rolling_max_lowered;
  std::vector<std::vector<MonoDequeEntryLowered>> rolling_max_lowered_buffers;
  std::vector<CrossStateLowered> cross_lowered;
  std::vector<Kalman1dStateLowered> kalman1d_lowered;
  std::vector<VwapStateLowered> vwap_lowered;
  std::vector<std::vector<double>> vwap_price_lowered_buffers;
  std::vector<std::vector<double>> vwap_vol_lowered_buffers;
  std::vector<RollingPairStateLowered> rolling_corr_lowered;
  std::vector<std::vector<double>> rolling_corr_x_lowered_buffers;
  std::vector<std::vector<double>> rolling_corr_y_lowered_buffers;
  std::vector<RollingPairStateLowered> rolling_beta_lowered;
  std::vector<std::vector<double>> rolling_beta_x_lowered_buffers;
  std::vector<std::vector<double>> rolling_beta_y_lowered_buffers;
};

class MultiSymbolSignalContext {
 public:
  explicit MultiSymbolSignalContext(std::size_t n_symbols = 1) : arena_(n_symbols) { RefreshParamViews(); }

  std::size_t NumSymbols() const { return arena_.size(); }
  void Resize(std::size_t n_symbols) {
    arena_.resize(n_symbols);
    RefreshParamViews();
  }

  SignalContext& PerSymbol(std::uint32_t symbol_id) { return arena_.at(static_cast<std::size_t>(symbol_id)); }
  const SignalContext& PerSymbol(std::uint32_t symbol_id) const { return arena_.at(static_cast<std::size_t>(symbol_id)); }

  void SetParameters(std::vector<double> params) {
    params_ = std::move(params);
    RefreshParamViews();
  }

  std::vector<double>& Parameters() { return params_; }
  const std::vector<double>& Parameters() const { return params_; }

 private:
  void RefreshParamViews() {
    const double* data = params_.empty() ? nullptr : params_.data();
    const std::size_t count = params_.size();
    for (auto& ctx : arena_) {
      ctx.params = data;
      ctx.num_params = count;
      ctx.gradient_param_count = count;
    }
  }

  std::vector<SignalContext> arena_;
  std::vector<double> params_;
};

using ProgramStepFn = void (*)(const MarketState*, MultiSymbolSignalContext*, std::uint32_t, double*);
void EvaluateAllSymbols(
    const std::vector<MarketState>& per_symbol_market,
    MultiSymbolSignalContext& arena,
    ProgramStepFn fn,
    double* outputs,
    std::size_t outputs_per_symbol);

class SymbolTable {
 public:
  std::size_t RegisterOrGetId(const std::string& symbol);
  std::size_t LookupId(const std::string& symbol) const;

 private:
  std::unordered_map<std::string, std::size_t> symbol_to_id_;
  std::size_t next_id_ = 0;
};

void RingStatsPush(RingStatsState& state, std::size_t period, double sample);
void RingStatsPushPrepared(RingStatsState& state, double sample);
double RingStatsMean(const RingStatsState& state);
double RingStatsStddevSample(const RingStatsState& state);
// Two-pass long-double sum-of-squared-deviations stddev. Kept exposed so
// `welford_stddev_parity_test` can gate the incremental Welford path
// against it; the production hot path (`jit_rt_rolling_std`,
// `jit_rt_zscore`) goes through the O(1) `RingStatsStddevSample`.
double RingStatsStddevSampleTwoPassReference(const RingStatsState& state);
bool RingStatsFull(const RingStatsState& state);
void VwapPush(VwapState& state, std::size_t period, double price, double volume);
void VwapPushPrepared(VwapState& state, double price, double volume);
double VwapValue(const VwapState& state);
bool VwapFull(const VwapState& state);
void LagPush(LagState& state, std::size_t period, double sample);
void LagPushPrepared(LagState& state, double sample);
double LagValue(const LagState& state);

// P7: paired-series rolling stats. Pushes a single (x, y) pair into the
// state's ring buffer and maintains the running sums. Returns the
// rolling Pearson correlation in [-1, 1], or NaN until `period` samples
// are observed.
void RollingPairPush(RollingPairState& state, std::size_t period, double x, double y);
double RollingPairCorrelation(const RollingPairState& state);
double RollingPairBeta(const RollingPairState& state);
bool RollingPairFull(const RollingPairState& state);

// P7: single-step 1-D Kalman filter update. `q` and `r` are the process
// and measurement noise variances. Returns the posterior estimate.
double Kalman1dStep(Kalman1dState& state, double x, double q, double r);
void EnsureNodeCapacity(SignalContext& ctx, std::size_t node_id);
void RefreshLoweredStateBases(SignalContext& ctx);
void PrewarmSignalContext(SignalContext& ctx, const SignalDef& signal);
void PrewarmSignalContext(MultiSymbolSignalContext& arena, std::uint32_t symbol_id, const SignalDef& signal);
void SetStandaloneParameters(SignalContext& ctx, const std::vector<double>& params);
double UpdateRollingMin(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample);
double UpdateRollingMax(MonoDequeState& dq, std::size_t& idx, std::size_t period, double sample);
double UpdateRollingMinPrepared(MonoDequeState& dq, std::size_t& idx, double sample);
double UpdateRollingMaxPrepared(MonoDequeState& dq, std::size_t& idx, double sample);

extern "C" {
SignalContext* jit_rt_symbol_ctx(MultiSymbolSignalContext* arena, std::uint32_t symbol_id);
double jit_rt_param(SignalContext* ctx, std::int64_t param_id);

double jit_rt_mid(const MarketState* state, std::int64_t symbol_id);
double jit_rt_bid(const MarketState* state, std::int64_t symbol_id);
double jit_rt_ask(const MarketState* state, std::int64_t symbol_id);
double jit_rt_spread(const MarketState* state, std::int64_t symbol_id);

double jit_rt_ema(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_ema_alpha(SignalContext* ctx, std::int64_t node_id, double x, double alpha, std::int64_t period);
double jit_rt_sma(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
bool jit_rt_sma_prepare(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    std::int64_t period,
    const double** buffer_out,
    std::int64_t* size_out);
double jit_rt_rolling_std(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_zscore(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_rolling_min(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_rolling_max(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_vwap(const MarketState* state, SignalContext* ctx, std::int64_t node_id, std::int64_t symbol_id, std::int64_t period);
double jit_rt_lag(SignalContext* ctx, std::int64_t node_id, double x, std::int64_t period);
double jit_rt_cross_above(SignalContext* ctx, std::int64_t node_id, double a, double b);
double jit_rt_cross_below(SignalContext* ctx, std::int64_t node_id, double a, double b);

// P7: new operator runtime entry points. The JIT routes calls to these
// helpers directly (no IR lowering yet); the interpreter calls the
// underlying C++ helpers above. Both paths share the same state
// structures, so interpreter and JIT are guaranteed bit-identical
// (the parity test gates this).
double jit_rt_rolling_corr(SignalContext* ctx, std::int64_t node_id, double x, double y, std::int64_t period);
double jit_rt_rolling_beta(SignalContext* ctx, std::int64_t node_id, double x, double y, std::int64_t period);
double jit_rt_kalman1d(SignalContext* ctx, std::int64_t node_id, double x, double q, double r);

// Phase 3 autodiff runtime helpers. Each updates the shared primal state once
// and carries the sensitivity for one selected param_id through the same
// recurrence, returning the primal value and writing the gradient via out-param.
double jit_rt_ema_alpha_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double alpha,
    double alpha_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_sma_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_lag_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_rolling_std_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_zscore_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_rolling_corr_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double y,
    double y_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_rolling_beta_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double y,
    double y_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_kalman1d_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    double q,
    double q_grad,
    double r,
    double r_grad,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_rolling_min_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_rolling_max_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double x,
    double x_grad,
    std::int64_t period,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_cross_above_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double a,
    double b,
    std::int64_t param_id,
    double* grad_out);
double jit_rt_cross_below_grad(
    SignalContext* ctx,
    std::int64_t node_id,
    double a,
    double b,
    std::int64_t param_id,
    double* grad_out);

// P0 lowered-state base accessors. Each returns a pointer to the first
// element of the corresponding lowered-state array. The JIT IR loads each
// of these once per function and indexes by node_id to read/write state
// directly without any per-op runtime call.
SmaStateLowered* jit_rt_sma_lowered_base(SignalContext* ctx);
EmaStateLowered* jit_rt_ema_lowered_base(SignalContext* ctx);
LagStateLowered* jit_rt_lag_lowered_base(SignalContext* ctx);
RollingStdStateLowered* jit_rt_rolling_std_lowered_base(SignalContext* ctx);
RollingStdStateLowered* jit_rt_zscore_lowered_base(SignalContext* ctx);
RollingMinMaxStateLowered* jit_rt_rolling_min_lowered_base(SignalContext* ctx);
RollingMinMaxStateLowered* jit_rt_rolling_max_lowered_base(SignalContext* ctx);
CrossStateLowered* jit_rt_cross_lowered_base(SignalContext* ctx);
Kalman1dStateLowered* jit_rt_kalman1d_lowered_base(SignalContext* ctx);
VwapStateLowered* jit_rt_vwap_lowered_base(SignalContext* ctx);
RollingPairStateLowered* jit_rt_rolling_corr_lowered_base(SignalContext* ctx);
RollingPairStateLowered* jit_rt_rolling_beta_lowered_base(SignalContext* ctx);
}

}  // namespace jitse
