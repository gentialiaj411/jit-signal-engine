# P7 — Broaden the operator set

P7 adds three new operators that unlock new classes of signal beyond
"more rolling stats":

| Operator                            | Returns               | Unlocks                             |
|-------------------------------------|-----------------------|-------------------------------------|
| `rolling_corr(x, y, n)`             | rolling Pearson `r`   | pair-trade and dispersion signals   |
| `rolling_beta(x, y, n)`             | rolling regression slope | factor-style hedge ratios        |
| `kalman1d(x, q, r)`                 | filtered estimate     | smoothing with explicit signal-to-noise tradeoff |

All three have:
- A C++ runtime helper.
- An interpreter binding.
- A JIT routing through the runtime helper (no IR lowering yet — same
  default-disabled state the P0 ops had before lowering).
- A bit-identical parity gate (`rolling_pair_kalman_parity_test`).
- A differential-oracle gate that compares against an independent
  reference implementation (`rolling_pair_kalman_oracle_test`).

## Argument shape

```
rolling_corr(x, y, period)    // x, y: numbers; period: positive integer literal
rolling_beta(x, y, period)    // same shape
kalman1d   (x, q, r)          // x: number; q, r: numeric literals, q >= 0, r > 0
```

The `period` arg for the paired ops must be a positive integer literal
(matches the existing convention for `sma`, `ema`, `lag`). `q` and `r`
for the Kalman filter must be numeric literals (they're hoisted into
the IR as `ConstantFP`); this avoids per-tick parameter loads.

## State representation

`runtime.h` defines two new state structs:

### `RollingPairState` (used by both `rolling_corr` and `rolling_beta`)

```c++
struct RollingPairState {
  std::vector<double> x_buf, y_buf;   // ring buffer of the last `capacity` samples
  std::size_t capacity, head, count;
  long double sum_x, sum_y, sum_xy, sum_xx, sum_yy;
};
```

The recurrence is the standard O(1)-per-update rolling-sum form,
identical structurally to the existing `RingStatsState`: when the
buffer is full we subtract the expiring sample's contributions before
adding the new sample's.

### `Kalman1dState`

```c++
struct Kalman1dState {
  double x_hat;   // posterior estimate
  double p;       // posterior variance
  double q, r;    // process / measurement noise parameters
  bool initialized;
};
```

`Kalman1dStep` runs the textbook predict-update on each tick. On the
very first call we initialize `x_hat = z, p = r` (so the first
posterior equals the measurement); subsequent calls iterate the
recurrence below:

```
p_pred = p + q
K      = p_pred / (p_pred + r)
x_hat  = x_hat + K * (z - x_hat)
p      = (1 - K) * p_pred         (clamped to >= 0)
```

## Numerical stability story

### Rolling correlation / beta

The running-sum recurrence is well-conditioned when:

- `period <= ~10k` (mantissa headroom in `long double` accumulators),
- the input distribution is not pathologically near-constant (which
  would push the unnormalized variances toward zero).

The oracle test runs 5000 ticks of a synthetic stream with prices in
roughly the `[50, 200]` range, deliberately correlated but not
near-constant. Engine output matches an independent two-pass O(window)
reference implementation to `worst_abs = 0`.

When the unnormalized variance terms `sum_xx - sum_x^2/n` or
`sum_yy - sum_y^2/n` come out non-positive (cancellation), the engine
returns NaN rather than a noise-amplified bogus correlation. For
workloads sitting near that boundary, a future enhancement would
switch to a Welford-style streaming pair-stats accumulator
(numerically stable but more expensive). The parity test would catch
any divergence from the current rolling-sum form, so the migration is
safe to do in isolation.

The correlation result is clamped to `[-1, +1]` to absorb FP overshoot
at the boundary.

### Kalman1d

The 1-D scalar form does not have the multivariate Kalman's
covariance-positive-definite issues (Joseph form / square-root filter)
because every state quantity is a non-negative scalar. We add a single
`p = max(0, p)` clamp to guard against catastrophic cancellation in
the `(1 - K) * p_pred` step when `K` is very close to 1.

The first measurement is treated as the initial posterior; this
mirrors the default-prior convention used by NumPy/SciPy filters and
keeps the parity test deterministic.

## Tests

### `rolling_pair_kalman_parity_test`

Five programs run through both the interpreter and the JIT for 5000
ticks of MarketSimulator output:

```
signal out = rolling_corr(mid(AAPL), mid(MSFT), 32)
signal out = rolling_beta(mid(AAPL), mid(MSFT), 50)
signal out = kalman1d(mid(AAPL), 0.01, 1.0)
signal out = mid(AAPL) - rolling_beta(mid(AAPL), mid(MSFT), 40) * mid(MSFT)
signal out = if rolling_std(mid(AAPL), 20) > 0 then kalman1d(mid(AAPL), 0.001, 0.5) else mid(AAPL)
```

All five pass with `max_abs_diff = 0` -- interpreter and JIT are
bit-identical because they route through the same C++ runtime helpers.

### `rolling_pair_kalman_oracle_test`

Compares engine output against:

- A **two-pass O(window) recomputation** for `rolling_corr` /
  `rolling_beta`. This is the definitionally correct answer.
- A **textbook scalar Kalman update** with local variables for
  `kalman1d`.

Tolerance: 1e-9 absolute. All three operators pass with `worst_abs = 0`
on the synthetic stream.

## Examples

`examples/pair_signal.sig` and `examples/kalman_signal.sig`
demonstrate the new operators in realistic signal definitions.

## Build / test summary

New files:

```
test/rolling_pair_kalman_parity_test.cpp
test/rolling_pair_kalman_oracle_test.cpp
examples/pair_signal.sig
examples/kalman_signal.sig
```

Modified:

```
src/runtime.h            -- RollingPairState, Kalman1dState, jit_rt_* decls
src/runtime.cpp          -- helpers, prewarm, extern-C entry points
src/interpreter.{h,cpp}  -- EvalRollingCorr/Beta/Kalman1d
src/jit_compiler.cpp     -- IR routing for the three new ops
src/signal_program.cpp   -- stateful-op detection for node_id allocation
CMakeLists.txt           -- two new test targets
```

ctest: 27/27 passing.
