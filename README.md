![CI](https://github.com/gentialiaj411/jit-signal-engine/actions/workflows/ci.yml/badge.svg)

A JIT compiler for financial trading signals, written in C++20 with LLVM. Parses a custom DSL, lowers it through a typed AST and LLVM IR, and emits optimized x86-64 machine code via LLVM ORC JIT.

## Architecture

```text
.sig file
    |
    v
Lexer --> Parser --> AST
                  |
         AllocateNodeIds()
                  |
         InlineSignalDependencies()
                  |
       +----------+-----------+
       |                      |
  Interpreter           JIT Compiler
  (tree-walk,           (LLVM IR gen
   ExprVisitor)          -> ORC JIT
                          -> x86-64)
       |                      |
       +----------+-----------+
                  |
            double output/tick
```

## Quick Start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/[Release/]jit_signal_engine examples/filtered_momentum.sig
```

## DSL Example

```sig
# Adaptive momentum signal with volatility filter
# Long when short EMA > long EMA AND vol is low
signal short_ma = ema(mid(AAPL), 10)
signal long_ma = ema(mid(AAPL), 60)
signal vol = rolling_std(mid(AAPL), 30)
signal raw = short_ma - long_ma
signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0
```

## Benchmark Results

| Mode         | Signal             | Throughput    | p50    | p99     |
|---|---|---|---|---|
| Interpreter  | filtered_momentum  | 1.27M ev/s    | 756 ns | 1809 ns |
| Interpreter  | zscore             | 2.00M ev/s    | 473 ns | 950 ns  |
| JIT          | (requires Linux+LLVM 17) | -       | -      | -       |

JIT benchmark requires Linux + LLVM 17. See benchmarks.md.
Run `bench/program_benchmark` to compare single-signal JIT vs multi-signal program compilation (cross-signal CSE path).

## Built-in Functions

| Function | Description |
|---|---|
| `mid` | Returns midpoint price `(bid + ask) / 2` for a ticker. |
| `bid` | Returns current best bid price for a ticker. |
| `ask` | Returns current best ask price for a ticker. |
| `spread` | Returns current spread `ask - bid` for a ticker. |
| `ema` | Computes exponential moving average over an input expression. |
| `sma` | Computes simple moving average over an input expression. |
| `rolling_std` | Computes rolling sample standard deviation over an input expression. |
| `rolling_min` | Computes rolling minimum over an input expression. |
| `rolling_max` | Computes rolling maximum over an input expression. |
| `zscore` | Computes rolling z-score of an input expression. |
| `vwap` | Computes rolling volume-weighted average price for a ticker. |
| `lag` | Returns the value from `period` ticks earlier. |
| `cross_above` | Emits `1` when first series crosses above second series on this tick, else `0`. |
| `cross_below` | Emits `1` when first series crosses below second series on this tick, else `0`. |
