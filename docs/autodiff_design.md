# Autodiff Design

## Phase 0 Scope

This document is the Phase 0 design gate for `AUTODIFF_TASK.md`. It resolves the parameter surface, time-direction choice, non-smooth policy, and Phase 4 objective, and it records the source-verified answers to `[CONFIRM-1..3]`.

This is design only. No implementation code is part of Phase 0.

## Current Ground Truth

- The AST today has `NumberLiteral`, `IdentifierExpr`, `UnaryOp`, `BinaryOp`, `FunctionCall`, and `Conditional`; there is no parameter node and no program-level parameter table. `NumberLiteral` carries raw `double value` and bare identifiers are currently invalid expressions at evaluation time.
- `signal_program` assigns stable `node_id` values only to stateful `FunctionCall` nodes. Structural dedup is limited to those stateful calls and only within one signal body, with unconditional-first aliasing.
- Runtime state is dense and node-indexed in `SignalContext`. JIT signatures already route through `MultiSymbolSignalContext*`, which is the natural place to hang shared parameter storage without changing the call ABI.
- `ema(period)` uses integer period and computes `alpha = 2 / (period + 1)` internally in the interpreter. `runtime.cpp` also already exposes `jit_rt_ema_alpha(...)`, which is a useful existing seam for a continuous-alpha path.
- `rolling_std` / `zscore` use the exact rolling Welford recurrences in `runtime.cpp`, including the periodic full-buffer refresh.
- `kalman1d` uses the scalar predict/update recurrence in `runtime.cpp` with first-sample initialization `x_hat = x`, `p = r`.

## `[CONFIRM-*]` Resolutions

### `[CONFIRM-1]` Recorded-data calibration objective is still broken at HEAD

Confirmed. The checked-in artifact [bench/results/backtest/determinism_a/ic_report.json](/abs/path/C:/Users/bhask/Documents/PROJECTS/jit-signal-engine/bench/results/backtest/determinism_a/ic_report.json) contains:

- `n_symbols = 8714`, `n_events = 200000`
- all IC values are `nan`
- all sample sizes are `0`

Also, [test/backtest_determinism_test.cpp](/abs/path/C:/Users/bhask/Documents/PROJECTS/jit-signal-engine/test/backtest_determinism_test.cpp) only compares the IC sections of two reruns for equality. Two identical broken runs still pass. So the current backtest gate is determinism-only, not correctness of the objective.

Decision for D4: Phase 4 must use a checked-in CSV fixture objective as the primary, reproducible calibration target. Recorded-data IC stays secondary and only runs after a separate fix/verification of `backtest_runner`.

### `[CONFIRM-2]` No parameter mechanism exists yet

Confirmed.

- Literals are represented as `NumberLiteral(double value)` in [src/ast.h](/abs/path/C:/Users/bhask/Documents/PROJECTS/jit-signal-engine/src/ast.h).
- The parser constructs literals directly as `NumberLiteral(tok.number_value)` in `parser.cpp`.
- `signal_program` and the interpreter have no parameter table.
- Several operators currently require numeric literals specifically:
  - integer periods for `ema`, `sma`, `rolling_std`, `zscore`, `vwap`, `lag`, `rolling_min/max`, `rolling_corr/beta`
  - `kalman1d(q, r)` currently requires numeric literals for `q` and `r`

### `[CONFIRM-3]` Structural dedup does not currently merge repeated parameter uses

Confirmed, and this is a brief-vs-source correction.

`AllocateNodeIds` / `AllocateProgramNodeIds` only assign/dedup `node_id` for stateful `FunctionCall` nodes. They do not dedup literals, identifiers, or any future parameter leaf by structure. Therefore two uses of the same parameter will remain two AST leaves unless we add a new dedup pass.

Design consequence: gradient accumulation must be keyed by `param_id`, not by AST node identity. If `alpha` is used twice in one expression, both reverse contributions must sum into the same gradient slot even though the AST contains two leaf nodes.

Phase 1 must add a direct fan-out test for this case.

## Decision Summary

### D1 - Parameter surface

Chosen:

- Add explicit top-level declarations:

```sig
param alpha_fast = 0.25
param alpha_slow = 0.05
param q = 0.01
param r = 0.10

signal fast = ema_alpha(mid(AAPL), alpha_fast)
signal slow = ema_alpha(mid(AAPL), alpha_slow)
signal filt = kalman1d(mid(AAPL), q, r)
```

- Parameters are opt-in. Existing numeric literals remain constants.
- Integer window periods remain numeric literals only and are not differentiable.
- The AST gets a dedicated `ParameterExpr` leaf plus a program-level `ParamDef { name, default_value, param_id }`.
- Parsed programs become `ProgramDef { params, signals }`. Keep a compatibility wrapper for signal-only callers during migration.
- Runtime storage is one dense parameter vector shared across the whole compiled program, attached to `MultiSymbolSignalContext` and exposed to per-symbol `SignalContext` as a non-owning view/pointer. This preserves the current JIT function ABI and avoids copying the same calibration parameters into every symbol slot.

Why this shape:

- It makes the differentiable surface explicit.
- It preserves existing interpreter semantics for non-parameterized programs.
- It avoids the semantic ambiguity of reusing `IdentifierExpr` for both signal refs and params.
- It keeps parameter values shared across symbols, which matches calibration semantics.

Additional operator decision:

- Keep existing `ema(expr, period)` unchanged and non-differentiable with respect to `period`.
- Add `ema_alpha(expr, alpha)` as the continuous EMA form used by autodiff and calibration.

This is the cleanest way to respect the existing "periods are integers" rule while still supporting exact EMA sensitivity.

Rejected alternatives:

- Make every literal differentiable:
  rejected because it silently changes the meaning of existing programs and would incorrectly expose window sizes and unrelated constants as trainable.
- Reuse `IdentifierExpr` for params:
  rejected because `IdentifierExpr` currently means "signal reference before inlining" and is invalid at evaluation time. Overloading that node would make inlining/binding rules fragile.
- Store parameters separately inside every `SignalContext`:
  rejected because multi-symbol execution would duplicate shared calibration state per symbol and create sync risk.

### D2 - Time direction

Chosen:

- Reverse-mode over the expression DAG within one tick.
- Forward sensitivity propagation over time for recurrent/windowed state.

Interpretation:

- Stateless algebra within a tick uses standard reverse accumulation.
- Stateful operators carry extra derivative state forward tick-to-tick for each active parameter.
- No per-tick tape allocation is allowed.

Why this shape:

- True BPTT needs tick-history storage or checkpointing over the whole run, which conflicts with the engine's streaming, warmed, allocation-free hot path.
- The runtime already has dense node-indexed state. Sensitivity state can use the same dense layout.
- Parameter counts here are expected to be small, so `O(P)` extra state is acceptable.

Tradeoff:

- Cost scales linearly with parameter count `P`.
- This design targets "a few to a few dozen" trainable parameters, not hundreds.

Rejected alternative:

- Full reverse-mode through time with tape/checkpointing:
  rejected because it changes the memory shape from fixed warm state to history-dependent allocation and would directly fight `hot_path_allocation_test`.

### D3 - Non-smooth policy

Chosen:

- `rolling_min` / `rolling_max`: route subgradient to the active extremum sample.
- Tie policy follows current runtime behavior:
  - `rolling_min` pops `>=`, so the most recent equal minimum wins.
  - `rolling_max` pops `<=`, so the most recent equal maximum wins.
- `cross_above`, `cross_below`, comparisons, and the condition branch of `if then else`: stop-gradient through the predicate.
- `if then else` still propagates the selected branch gradient piecewise:
  - if condition true: result gradient = then-branch gradient
  - else: result gradient = else-branch gradient
  - derivative of the condition itself is treated as zero

Why:

- This matches the engine's discrete semantics and avoids inventing smooth surrogates.
- Away from switching boundaries, branch-selected piecewise derivatives are exact.

Rejected alternative:

- Smooth surrogate versions of compare/cross/min/max:
  rejected for Phase 0 because they would change DSL semantics and need explicit human approval.

### D4 - Phase 4 objective

Chosen:

- Primary objective: checked-in CSV fixture fit-to-target objective.
- Secondary objective: recorded-data IC only after `backtest_runner` is separately fixed and verified.

Why:

- The fixture path is reproducible in CI and currently the only trustworthy calibration target.
- Recorded-data IC at HEAD is not currently evidence-backed.

Rejected alternative:

- Make recorded-data IC the primary Phase 4 demo now:
  rejected because the current checked-in artifact is all-NaN with zero samples.

## Differentiation Model

## Primal/Adjoint Structure

For each output signal on a tick:

1. Evaluate the primal graph and record fixed-size per-node primal values in preallocated scratch.
2. Seed reverse accumulation at the output node with adjoint `1`.
3. Reverse over stateless intra-tick edges.
4. For stateful operators, inject the exact forward sensitivity recurrence for each parameter into extra persistent state slots.

This is not a tape interpreter. The graph is static, scratch is pre-sized at compile/prewarm time, and stateful sensitivities live in dense runtime arrays.

## Fan-out rule

If a node feeds multiple consumers, incoming adjoints sum:

`adj[v] = sum over users u of adj[u] * d u / d v`

For parameters this is mandatory even without structural dedup. Two `ParameterExpr(alpha)` leaves with the same `param_id` both contribute to `grad[param_id]`.

## Operator Coverage and State Inventory

### Stateless

- Arithmetic: `+ - * /`
- Unary/math: unary `+/-`, `abs`, `log`, `sqrt`
- Market reads: `mid`, `bid`, `ask`, `spread`
- Comparisons / logicals / `if`: piecewise rules per D3

Persistent derivative state needed: none.

### Windowed linear

- `lag(x, period)`
  - derivative is the same `lag` applied to the input sensitivity stream
  - extra state: one lag ring per `(node_id, param_id)`
- `sma(x, period)`
  - derivative is the same `sma` applied to the input sensitivity stream
  - extra state: one SMA/ring state per `(node_id, param_id)`
- `vwap(symbol, period)`
  - with current DSL surface, this node has no differentiable parameter input other than forbidden integer `period`
  - local derivative w.r.t. parameters is zero
  - no extra sensitivity state needed unless a future DSL surface adds differentiable price/volume inputs

### Recurrent

- `ema_alpha(x, alpha)`
  - extra state: one scalar sensitivity `dvalue/dtheta_i` per `(node_id, param_id)`
- `kalman1d(x, q, r)`
  - extra state: two scalars per `(node_id, param_id)`:
    - `dx_hat/dtheta_i`
    - `dp/dtheta_i`

### Nonlinear windowed

- `rolling_std(x, period)`
  - extra state per `(node_id, param_id)` matching the primal rolling Welford state:
    - derivative ring buffer for past input sensitivities
    - `dsum`, `dmean`, `dm2`
    - derivative refresh path when the primal refreshes
- `zscore(x, period)`
  - same derivative state as `rolling_std`, then chain rule through `(x - mean) / stddev`
- `rolling_corr(x, y, period)`
  - extra state per `(node_id, param_id)`:
    - derivative ring buffers for `x` and `y` sensitivities
    - `d(sum_x)`, `d(sum_y)`, `d(sum_xy)`, `d(sum_xx)`, `d(sum_yy)`
- `rolling_beta(x, y, period)`
  - same state shape as `rolling_corr`

### Non-smooth

- `rolling_min`, `rolling_max`
  - need arg-extremum tracking from the primal state
  - need stored past input sensitivities so the chosen extremum's sensitivity can be retrieved on later ticks
  - this requires either:
    - a sensitivity ring buffer aligned with the primal sample stream, or
    - sensitivity carried alongside deque entries
  - derivative routes to the chosen extremum input sensitivity
- `cross_above`, `cross_below`
  - output gradient zero by policy

## Exact Derivations

### EMA with continuous alpha

Use the continuous form:

`e_t = alpha * x_t + (1 - alpha) * e_(t-1)`

For a parameter `theta`,

`d e_t / d theta`
`= d alpha / d theta * x_t + alpha * d x_t / d theta`
`  - d alpha / d theta * e_(t-1) + (1 - alpha) * d e_(t-1) / d theta`

Rearranged:

`d e_t / d theta = alpha * x'_t + (1 - alpha) * e'_(t-1) + alpha' * (x_t - e_(t-1))`

Special cases:

- if `theta` is `alpha` itself, `alpha' = 1`, so

`d e_t / d alpha = x'_t * alpha + (1 - alpha) * e'_(t-1) + (x_t - e_(t-1))`

- if the input stream does not depend on `alpha`, `x'_t = 0`, giving

`d e_t / d alpha = (x_t - e_(t-1)) + (1 - alpha) * d e_(t-1) / d alpha`

which is the brief's target formula.

Initialization:

- primal first tick: `e_0 = x_0`
- sensitivity first tick: `e'_0 = x'_0`

New state slot per parameter:

- `ema_sens[node_id][param_id] = d value / d theta`

### Kalman1d

Match `runtime.cpp` exactly.

Primal recurrence for initialized state:

- `p_pred = p + q`
- `denom = p_pred + r`
- `K = p_pred / denom`
- `innovation = x - x_hat`
- `x_hat_new = x_hat + K * innovation`
- `p_new = (1 - K) * p_pred`
- then clamp `p_new = max(0, p_new)`

For parameter `theta`, let:

- `x' = d x / d theta`
- `x_hat' = d x_hat / d theta`
- `p' = d p / d theta`
- `q' = d q / d theta`
- `r' = d r / d theta`

Then:

- `p_pred' = p' + q'`
- `denom' = p_pred' + r'`
- `K' = (p_pred' * denom - p_pred * denom') / denom^2`
- `innovation' = x' - x_hat'`
- `x_hat_new' = x_hat' + K' * innovation + K * innovation'`
- `p_new' = (-K') * p_pred + (1 - K) * p_pred'`

Initialization:

- first tick primal: `x_hat = x`, `p = r`
- first tick sensitivities:
  - `x_hat' = x'`
  - `p' = r'`

Clamp handling:

- valid calibrated configs keep `q >= 0`, `r > 0`, so `denom > 0`
- if `denom <= 0`, the current runtime skips the update and returns the prior `x_hat` unchanged
  - sensitivity must take the same branch:
    - `x_hat_new' = x_hat'`
    - `p_new' = p'`
- if the existing runtime clamp activates and `p_new < 0`, the sensitivity follows the active branch:
  - unclamped branch if `p_new > 0`
  - zero derivative through the clamp if `p_new <= 0`

New state slots per parameter:

- `kalman_dxhat[node_id][param_id]`
- `kalman_dp[node_id][param_id]`

### Rolling Welford `rolling_std`

Differentiate the implemented rolling Welford updates, not the textbook variance closed form.

Notation:

- primal state before update: `mean`, `m2`, `count`
- sensitivity state: `mean'`, `m2'`
- current input sensitivity: `x'`
- expired sample sensitivity on slide: `old'`

#### Add path

When the window is still filling, after increment `count = n`:

- `delta = x - mean_prev`
- `mean = mean_prev + delta / n`
- `m2 = m2_prev + delta * (x - mean)`

Differentiate:

- `delta' = x' - mean_prev'`
- `mean' = mean_prev' + delta' / n`
- `m2' = m2_prev' + delta' * (x - mean) + delta * (x' - mean')`

#### Slide path

When the window is full, the implementation does remove-then-add.

Remove old sample:

- `delta_r = old - mean_prev`
- `mean_r = mean_prev - delta_r / (n - 1)`
- `m2_r = m2_prev - delta_r * (old - mean_r)`

Differentiate:

- `delta_r' = old' - mean_prev'`
- `mean_r' = mean_prev' - delta_r' / (n - 1)`
- `m2_r' = m2_prev' - delta_r' * (old - mean_r) - delta_r * (old' - mean_r')`

Add new sample:

- `delta_a = x - mean_r`
- `mean = mean_r + delta_a / n`
- `m2 = m2_r + delta_a * (x - mean)`

Differentiate:

- `delta_a' = x' - mean_r'`
- `mean' = mean_r' + delta_a' / n`
- `m2' = m2_r' + delta_a' * (x - mean) + delta_a * (x' - mean')`

Implemented branch conventions that must be mirrored exactly:

- `runtime.cpp` has a degenerate `capacity <= 1` snap path in the slide helper:
  - primal: `mean = x`, `m2 = 0`
  - sensitivity: `mean' = x'`, `m2' = 0`
- `runtime.cpp` clamps negative `m2` after the slide-path update:
  - if unclamped `m2 > 0`, use the normal derivative above
  - if the clamp activates, take the branch derivative `m2' = 0`

Readout:

- sample variance `var = m2 / (n - 1)`
- for `n < 2`, output is `NaN`
- derivative for valid `n >= 2`:
  - `var' = m2' / (n - 1)`
  - `std = sqrt(var)`
  - `std' = var' / (2 * sqrt(var))`

Refresh path:

`runtime.cpp` periodically recomputes `mean` and `m2` from the whole ring buffer after `capacity` slides. The sensitivity path must refresh on the same ticks from the derivative buffer, or parity will drift.

New state slots per parameter:

- derivative ring buffer for input sensitivities
- `rolling_std_dsum`, because `RingStatsMean` currently reads from `sum`
- `rolling_std_dmean`
- `rolling_std_dm2`
- derivative `slides_since_refresh` mirrors the primal schedule but is not itself differentiable

Precision note:

- primal Welford state uses `long double`
- sensitivity accumulators for `dsum`, `dmean`, `dm2`, and refresh recompute should also use `long double`
- otherwise the planned Phase 3 `1e-12` compiled-vs-interpreted gate is likely unreachable for `rolling_std` / `zscore`

### Z-score

Primal:

- `z = (x - mean) / std`

Derivative:

- `z' = (x' - mean') / std - (x - mean) * std' / std^2`

Warm-up / zero-variance behavior must match the interpreter exactly:

- if the primal returns `NaN` because the window is not full or `abs(std) < 1e-18`, the gradient output is also `NaN` and the test oracle must compare that convention explicitly.

### Rolling correlation and beta

The current runtime maintains:

- `sum_x`, `sum_y`, `sum_xy`, `sum_xx`, `sum_yy`

For each parameter we mirror:

- `d sum_x`, `d sum_y`, `d sum_xy`, `d sum_xx`, `d sum_yy`

Updates are exact termwise derivatives of the push/pop recurrence. Examples:

- `d(sum_xy) += x' * y + x * y'`
- `d(sum_xx) += 2 * x * x'`

and subtract the expired sample contributions on slide with the same formulas using `old_x`, `old_y`, `old_x'`, `old_y'`.

Readout:

- `cov = sum_xy - sum_x * sum_y / n`
- `var_x = sum_xx - sum_x^2 / n`
- `var_y = sum_yy - sum_y^2 / n`
- `corr = cov / sqrt(var_x * var_y)`
- `beta = cov / var_x`

Derivatives follow directly from quotient and chain rule on those exact readout formulas.

### Rolling min / max

Let `i*` be the active extremum index selected by the primal deque state using the current tie rule.

- `rolling_min' = x'_(i*)`
- `rolling_max' = x'_(i*)`

Because `i*` may reference an older sample, Phase 2 must store historical input sensitivities for these operators. Arg-extremum tracking alone is insufficient.

Warm-up must mirror the primal:

- until the window is full, primal output is `NaN`
- gradient output is also `NaN`

### Cross operators and comparisons

By policy:

- `d/dtheta cross_above = 0`
- `d/dtheta cross_below = 0`
- predicate gradients are zero

For `if cond then a else b`:

- if `cond != 0`: derivative is `a'`
- else: derivative is `b'`

## Testing Consequences

Phase 1 and beyond should implement these test requirements:

- central finite-difference oracle:
  - `h = 1e-6 * max(1, |theta|)`
  - smooth ops: relative error `<= 1e-4` or absolute error `<= 1e-8` near zero
- compiled gradient vs interpreted adjoint:
  - relative error `<= 1e-12`, mirroring the value-parity discipline
- explicit fan-out case:
  - one parameter used more than once in the same expression
  - expected behavior: both contributions sum into one gradient slot
- finite-difference checks for piecewise or clamped operators must avoid switching boundaries
  - central differences that straddle a predicate flip, extremum tie flip, Welford clamp branch, or Kalman guard/clamp branch will disagree with the piecewise AD gradient by construction
  - the harness must either generate points known to be away from those boundaries or detect-and-skip those cases explicitly
  - do not paper over these with looser tolerances
- explicit warm-up boundary tests:
  - `lag`, `sma`, `rolling_std`, `zscore`, `rolling_min/max`, `rolling_corr/beta`
- explicit non-smooth convention tests:
  - tie behavior for `rolling_min/max`
  - zero gradient for `cross_*`
  - selected-branch-only gradient for `if`

## Implementation Guidance for Later Phases

- Reuse existing `node_id` allocation for stateful sensitivity state.
- Add a separate stable `expr_id` numbering for per-tick primal scratch and reverse adjoint scratch.
- Keep no-param programs on the existing fast path.
- The first calibrated EMA examples should use `ema_alpha`, not period-based `ema`.
- Phase 4 should add a checked-in CSV fixture and `calibration_smoke_test` before any recorded-data calibration claim.
- `ema_alpha` is a new primal DSL/operator surface, not just an autodiff helper
  - it must go through the normal new-operator checklist:
    - parser and type-check support
    - interpreter semantics
    - runtime helper usage and lowering decision
    - `StatefulLoweringFlags` coverage if lowered
    - fuzz-generator coverage
    - formatter/lint round-trip coverage
- `kalman1d` currently requires literal `q` and `r`
  - Phase 1 must explicitly relax that restriction to accept parameter leaves
  - this is intended scope, not an incidental parser side effect

## Phase 0 Verdict

- D1: explicit opt-in params with dedicated AST/program nodes; add continuous `ema_alpha`
- D2: reverse within tick, forward sensitivity through time
- D3: subgradient for rolling extrema; stop-gradient for discrete predicates
- D4: CSV fixture primary objective; recorded-data objective blocked pending backtest fix

This is the design proposed for approval before Phase 1 implementation.
