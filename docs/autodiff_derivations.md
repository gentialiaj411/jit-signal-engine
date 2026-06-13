# Autodiff Derivations (forward-mode sensitivities)

Companion to `docs/autodiff_design.md`. Every operator's sensitivity recurrence, derived
line-by-line from the primal update. For each quantity `v` in the primal, `v' ≡ dv/dθ` for a
chosen parameter `θ`. Parameters: `θ' = 1` for the parameter being differentiated, `0`
otherwise. Market reads (`mid/bid/ask/spread`) and integer periods are constants (`' = 0`)
unless they are themselves downstream of a parameter.

**Method (the only thing to remember):** apply `d/dθ` to *every line* of the primal update,
including init lines, guard branches, and clamps. Four rules: `(a+b)'=a'+b'`,
`(a·b)'=a'b+ab'`, `(a/b)'=(a'b−ab')/b²`, constants differentiate to 0.

Source-verified against `src/runtime.cpp`: EMA, Kalman (`:550-576`), Welford
(`:200-260`). The rest are derived from the standard primal recurrence and must be matched
to the exact code branch at implementation time (flagged inline).

---

## Stateless (no persistent derivative state)

Reverse/symbolic over the intra-tick DAG. `c` denotes a constant subexpression.

| primal | sensitivity |
|---|---|
| `a + b` | `a' + b'` |
| `a − b` | `a' − b'` |
| `a · b` | `a'·b + a·b'` |
| `a / b` | `(a'·b − a·b') / b²` |
| `−a` | `−a'` |
| `abs(a)` | `sign(a)·a'`  (subgradient `0` at `a=0`) |
| `log(a)` | `a' / a` |
| `sqrt(a)` | `a' / (2·sqrt(a))` |
| `mid/bid/ask/spread` | `0` |
| comparisons, `&&`, `||`, `!` | `0` (stop-gradient, per D3) |
| `if c then a else b` | `a'` if `c` else `b'` (predicate derivative `0`) |

---

## EMA — `ema_alpha(x, α)`  *(source-verified)*

Primal: `e_t = α·x_t + (1 − α)·e_{t−1}`, init `e_0 = x_0`.

Differentiate term by term:
```
e_t' = (α'·x_t + α·x_t') + (−α'·e_{t−1} + (1−α)·e_{t−1}')
```
Group the `α'` terms:
```
e_t' = α·x_t' + (1 − α)·e_{t−1}' + α'·(x_t − e_{t−1})
```
Init: `e_0' = x_0'`.

Case `θ = α` with input independent of `α` (`x_t' = 0`):
```
e_t' = (1 − α)·e_{t−1}' + (x_t − e_{t−1})
```
The first two terms are the EMA formula applied to the sensitivity (EMA is linear in its
inputs); the last term is the parameter's own contribution, equal to the EMA update
increment.

**State:** one scalar `e'` per `(node_id, param_id)`. Update right after `e`.

---

## Kalman1d — `kalman1d(x, q, r)`  *(source-verified, `runtime.cpp:550-576`)*

Primal (initialized branch):
```
p_pred = p + q
denom  = p_pred + r
[guard] if denom ≤ ε: return x_hat unchanged
K      = p_pred / denom
x_hat  = x_hat + K·(x − x_hat)
p_new  = (1 − K)·p_pred
[clamp] if p_new < 0: p_new = 0
```
Init (first tick): `x_hat = x`, `p = r`.

Let `innov = x − x_hat`. Differentiate each line:
```
p_pred'  = p' + q'
denom'   = p_pred' + r'
[guard]  if guard taken: x_hat', p' carry forward unchanged (skip the rest)
K'       = (p_pred'·denom − p_pred·denom') / denom²
innov'   = x' − x_hat'
x_hat'   = x_hat' + K'·innov + K·innov'      (use pre-update x_hat' in innov' and on RHS)
p_new'   = −K'·p_pred + (1 − K)·p_pred'
[clamp]  if p_new < 0: p_new' = 0
```
Init sensitivities: `x_hat' = x'`, `p' = r'`.

**Ordering hazard:** compute `innov'` and use the *old* `x_hat'` before overwriting it,
exactly as the primal uses the old `x_hat` in `innov`.

**State:** two scalars `x_hat'`, `p'` per `(node_id, param_id)`.

---

## Welford `rolling_std(x, period)`  *(source-verified, `runtime.cpp:200-260`)*

State: `mean`, `m2`, `count`; carry `mean'`, `m2'`. The `m2` lines use the **post-update**
mean — keep that straight. `n` is the window count.

### Add path (window filling), `runtime.cpp:200-203`
```
delta = x − mean_prev          →  delta' = x' − mean_prev'
mean  = mean_prev + delta/n     →  mean'  = mean_prev' + delta'/n
m2    = m2_prev + delta·(x−mean) →  m2'    = m2_prev' + delta'·(x−mean) + delta·(x'−mean')
```

### Slide path (full window: remove then add), `runtime.cpp:216-225`
Remove expired sample `old` (with stored sensitivity `old'`), divisor `n−1`:
```
delta_r = old − mean_prev        →  delta_r' = old' − mean_prev'
mean_r  = mean_prev − delta_r/(n−1) → mean_r' = mean_prev' − delta_r'/(n−1)
m2_r    = m2_prev − delta_r·(old−mean_r)
        →  m2_r' = m2_prev' − delta_r'·(old−mean_r) − delta_r·(old'−mean_r')
```
Add new sample, divisor `n`:
```
delta_a = x − mean_r             →  delta_a' = x' − mean_r'
mean    = mean_r + delta_a/n      →  mean'    = mean_r' + delta_a'/n
m2      = m2_r + delta_a·(x−mean)  →  m2'      = m2_r' + delta_a'·(x−mean) + delta_a·(x'−mean')
```

### Degenerate / clamp branches  *(must differentiate these too)*
- Snap-to-sample (`runtime.cpp:207-209`, count→1): `mean = x`, `m2 = 0` → `mean' = x'`, `m2' = 0`.
- `m2 < 0` clamp (`runtime.cpp:230`): on the clamped branch `m2' = 0`.

### Periodic refresh (`runtime.cpp:233-260`)
Recompute from the ring on the same tick the primal does, using the derivative ring:
```
mean = (1/n)·Σ buf[i]        →  mean' = (1/n)·Σ buf'[i]
m2   = Σ (buf[i] − mean)²     →  m2'   = Σ 2·(buf[i] − mean)·(buf'[i] − mean')
```

### Readout
```
var = m2/(n−1)   →  var' = m2'/(n−1)
std = sqrt(var)  →  std' = var' / (2·std)
```
For `n < 2`: primal `NaN`, sensitivity `NaN` (the FD oracle must compare this convention).

**State per `(node_id, param_id)`:** a derivative ring buffer (for `old'`), `mean'`, `m2'`.
Use `long double` to match the primal, or the 1e-12 compiled-vs-interpreted gate is
unreachable.

---

## Z-score `zscore(x, period)`

Primal `z = (x − mean)/std`, with `mean`/`std` from the Welford block above.
```
z' = (x' − mean')/std − (x − mean)·std'/std²
```
Warm-up / `|std| < 1e-18`: primal `NaN` → sensitivity `NaN`.

**State:** the `rolling_std` state above (no additional slots).

---

## SMA `sma(x, period)` and `lag(x, period)`  *(linear)*

`sma`: maintain `sum`; `sum += x − old` on slide → `sum' += x' − old'`; output `sum/n` →
`sum'/n`. Equivalently, the SMA of the input-sensitivity stream.

`lag`: `out_t = x_{t−k}` → `out_t' = x'_{t−k}`.

**State:** one derivative ring per `(node_id, param_id)` (needed for `old'` / delayed `x'`).

---

## Rolling correlation / beta `rolling_corr/beta(x, y, period)`

> Match the exact sum-maintenance code at implementation time; derived here from the
> standard five-accumulator form the design doc cites.

Maintained sums and their sensitivities on push `(x, y)`:
```
sum_x  += x      →  sum_x'  += x'
sum_y  += y      →  sum_y'  += y'
sum_xy += x·y    →  sum_xy' += x'·y + x·y'
sum_xx += x·x    →  sum_xx' += 2·x·x'
sum_yy += y·y    →  sum_yy' += 2·y·y'
```
On pop, subtract the same forms using `old_x, old_y, old_x', old_y'`.

Readout (`n` = window count):
```
cov   = sum_xy − sum_x·sum_y/n
      →  cov'   = sum_xy' − (sum_x'·sum_y + sum_x·sum_y')/n
var_x = sum_xx − sum_x²/n
      →  var_x' = sum_xx' − 2·sum_x·sum_x'/n
var_y = sum_yy − sum_y²/n
      →  var_y' = sum_yy' − 2·sum_y·sum_y'/n

beta  = cov/var_x
      →  beta'  = (cov'·var_x − cov·var_x')/var_x²

D     = sqrt(var_x·var_y)
      →  D'     = (var_x'·var_y + var_x·var_y')/(2·D)
corr  = cov/D
      →  corr'  = cov'/D − cov·D'/D²
```

**State per `(node_id, param_id)`:** derivative rings for `x` and `y`, plus
`sum_x', sum_y', sum_xy', sum_xx', sum_yy'`.

---

## Rolling min / max  *(non-smooth subgradient)*

Output is the active extremum sample `buf[i*]`, where `i*` is the index the primal deque
selects under its tie rule (`rolling_min` keeps most-recent equal min; `rolling_max`
most-recent equal max).
```
rolling_min' = x'_{i*}     rolling_max' = x'_{i*}
```
Until the window is full: primal `NaN` → sensitivity `NaN`.

**State:** a derivative ring so `x'_{i*}` (a past sensitivity) is retrievable.

---

## VWAP `vwap(symbol, period)`

With the current DSL surface the only parameter position is the integer `period`
(non-differentiable), and price/volume come from market reads, so the local sensitivity is
`0`. General form if a future surface makes price/volume parameter-dependent: with
`P = Σ price·vol`, `V = Σ vol`, `vwap = P/V`,
```
P' += price'·vol + price·vol' ;  V' += vol' ;  vwap' = (P'·V − P·V')/V²
```

---

## Cross operators / predicates

Per D3, output gradient is `0`: `cross_above' = cross_below' = 0`, and all comparison /
boolean predicate gradients are `0`. (Away from switching boundaries this is exact; at a
boundary it is the chosen subgradient convention.)

---

## Verification convention (the oracle)

Every result above is checked, not trusted, by central finite differences:
```
g_fd = (f(θ + h) − f(θ − h)) / (2h),   h = 1e-6·max(1, |θ|)
```
Smooth ops: `|g_ad − g_fd| / max(1,|g_fd|) ≤ 1e-4` (or abs ≤ 1e-8 near zero). Compiled
gradient vs interpreted adjoint: rel ≤ 1e-12. FD checks must be evaluated at points away
from switching boundaries (predicate flips, min/max ties, the Kalman guard/clamp), or the
piecewise-exact AD gradient and the straddling FD estimate will disagree by construction.
