# DSL Reference (MVP)

## Grammar (EBNF)

```ebnf
program      = signal_def EOF ;
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

- Program must define exactly one signal.
- Implemented built-ins:
  - `mid(<ticker>)`
  - `bid(<ticker>)`
  - `ask(<ticker>)`
  - `spread(<ticker>)`
  - `ema(<expr>, <period>)` where period is a positive integer literal
  - `sma(<expr>, <period>)` where period is a positive integer literal
  - `rolling_std(<expr>, <period>)` where period is a positive integer literal
  - `zscore(<expr>, <period>)` where period is a positive integer literal
  - `rolling_min(<expr>, <period>)` where period is a positive integer literal
  - `rolling_max(<expr>, <period>)` where period is a positive integer literal
  - `vwap(<ticker>, <period>)` where period is a positive integer literal
  - `lag(<expr>, <period>)` where period is a positive integer literal
  - `cross_above(<expr>, <expr>)`
  - `cross_below(<expr>, <expr>)`
  - `abs(<expr>)`, `log(<expr>)`, `sqrt(<expr>)`
- Comparisons supported: `> < >= <= == !=` (return `1.0` for true, `0.0` for false).
- Logical operators supported: `&& ||` (both sides are always evaluated).
- Conditional supported: `if <cond> then <expr> else <expr>`.
- `<ticker>` must be an identifier token (`AAPL`, `MSFT`, etc.).
- All numeric values are `double`.

## Example

```text
signal spread = mid(AAPL) - mid(MSFT)
```

## Logical Operators

- `&&` returns `1.0` when both operands are non-zero, else `0.0`.
- `||` returns `1.0` when either operand is non-zero, else `0.0`.
- `!=` returns `1.0` when operands differ by at least `1e-12`, else `0.0`.

Example:

```text
signal filtered = if spread(AAPL) > 0.01 && rolling_std(mid(AAPL), 20) < 0.5 then ema(mid(AAPL), 5) - ema(mid(AAPL), 20) else 0
```

## Debugging

Dump generated LLVM IR for the selected signal:

```text
jit_signal_engine --dump-ir examples/zscore_signal.sig
```

`--dump-ir` prints post-optimization IR (after LLVM O2 passes).  
`--dump-ir-pre` prints pre-optimization IR (direct emitter output).
