# DSL Reference

## Grammar (EBNF)

```ebnf
program      = { param_def | signal_def } EOF ;
param_def    = "param" identifier "=" expr ;
signal_def   = "signal" identifier "=" expr ;

expr         = conditional ;
conditional  = "if" expr "then" expr "else" expr
             | logical ;
logical      = comparison { ("&&" | "||") comparison } ;
comparison   = additive { (">" | "<" | ">=" | "<=" | "==" | "!=") additive } ;
additive     = term { ("+" | "-") term } ;
term         = factor { ("*" | "/") factor } ;
factor       = number
             | function_call
             | "(" expr ")"
             | ("+" | "-") factor ;

function_call = identifier "(" arg_list? ")" ;
arg_list      = expr { "," expr } ;
```

## Semantic Rules (Pre-LLVM Stage)

- Programs may declare zero or more top-level `param` bindings followed by one or more `signal` definitions.
- `param <name> = <expr>` declares a scalar parameter with a folded numeric default value. Parameters are opt-in: ordinary numeric literals remain constants.
- Integer window lengths remain integer, non-differentiable literals. This applies to `ema`, `sma`, `rolling_std`, `zscore`, `rolling_min`, `rolling_max`, `vwap`, `lag`, `rolling_corr`, and `rolling_beta`.
- Implemented built-ins:
  - `mid(<ticker>)`
  - `bid(<ticker>)`
  - `ask(<ticker>)`
  - `spread(<ticker>)`
  - `ema(<expr>, <period>)` where `period` is a positive integer literal
  - `ema_alpha(<expr>, <alpha>)` where `alpha` is a scalar expression and must evaluate in `[0, 1]`
  - `sma(<expr>, <period>)` where `period` is a positive integer literal
  - `rolling_std(<expr>, <period>)` where `period` is a positive integer literal and `period >= 2`
  - `zscore(<expr>, <period>)` where `period` is a positive integer literal and `period >= 2`
  - `rolling_min(<expr>, <period>)` where `period` is a positive integer literal
  - `rolling_max(<expr>, <period>)` where `period` is a positive integer literal
  - `vwap(<ticker>, <period>)` where `period` is a positive integer literal
  - `lag(<expr>, <period>)` where `period` is a positive integer literal
  - `rolling_corr(<expr>, <expr>, <period>)`
  - `rolling_beta(<expr>, <expr>, <period>)`
  - `kalman1d(<expr>, <q>, <r>)`
  - `cross_above(<expr>, <expr>)`
  - `cross_below(<expr>, <expr>)`
  - `abs(<expr>)`, `log(<expr>)`, `sqrt(<expr>)`
- Comparisons supported: `> < >= <= == !=` (return `1.0` for true, `0.0` for false).
- Logical operators supported: `&& ||` (both sides are always evaluated).
- Conditional supported: `if <cond> then <expr> else <expr>`.
- `<ticker>` must be an identifier token (`AAPL`, `MSFT`, etc.).
- All numeric values are `double`.

## Parameters

Use top-level `param` declarations when a program needs externally tunable scalar values.

```sig
param alpha_fast = 0.25
param alpha_slow = 0.05
param bias = -0.10

signal fast = ema_alpha(mid(AAPL), alpha_fast)
signal slow = ema_alpha(mid(AAPL), alpha_slow)
signal out = fast - slow + bias
```

Parameter names share the program namespace with signals, so the same name cannot be used for both.

## `ema` vs `ema_alpha`

- `ema(expr, period)` is the classic period-based EMA surface. The runtime derives its smoothing factor from the integer `period`.
- `ema_alpha(expr, alpha)` is the continuous-alpha form. It uses the supplied `alpha` directly and rejects values outside `[0, 1]`.
- For autodiff and calibration, `ema_alpha` is the differentiable EMA surface. Period-based `ema` keeps its integer-window semantics.

## Example

```sig
param scale = 1.25
signal spread = scale * (mid(AAPL) - mid(MSFT))
```

## Logical Operators

- `&&` returns `1.0` when both operands are non-zero, else `0.0`.
- `||` returns `1.0` when either operand is non-zero, else `0.0`.
- `!=` returns `1.0` when operands differ by at least `1e-12`, else `0.0`.

Example:

```sig
signal filtered = if spread(AAPL) > 0.01 && rolling_std(mid(AAPL), 20) < 0.5 then ema(mid(AAPL), 5) - ema(mid(AAPL), 20) else 0
```

## Autodiff Notes

- The differentiable surface is opt-in through `param` declarations.
- Continuous parameter sensitivities are defined for stateless arithmetic and for the implemented stateful recurrences documented in `docs/autodiff_design.md`.
- `cross_above`, `cross_below`, comparisons, and boolean predicates use stop-gradient conventions in the autodiff path.
- The checked-in calibration demo is fixture-backed under `bench/results/autodiff/`. It does not claim the recorded-data IC/backtest calibration path works at HEAD.

## Debugging

Dump generated LLVM IR for the selected signal:

```text
jit_signal_engine --dump-ir examples/zscore_signal.sig
```

`--dump-ir` prints post-optimization IR (after LLVM O2 passes).  
`--dump-ir-pre` prints pre-optimization IR (direct emitter output).
