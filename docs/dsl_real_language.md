# P6 — DSL becomes a real language

P6 adds the three pieces that make the engine's DSL feel like a real
compiler front-end instead of a one-shot expression evaluator:

1. **Source locations on every token and AST node.** Errors now point
   at the offending token with a caret underline, like every modern
   compiler.
2. **An AST-level constant-folding pass.** `sma(mid(AAPL), 5 + 5)` and
   `sma(mid(AAPL), 10)` produce identical pre-opt IR.
3. **A static type system with `bool` distinct from `number`.** `if`
   conditions must be `bool`. Arithmetic on `bool` results, or
   `&&`/`||` on numbers, are now type errors.

All three are gated by unit tests in `test/source_location_test.cpp`,
`test/constant_fold_test.cpp`, and `test/type_check_test.cpp`.

---

## P6.1 — Source locations

A `SourceLoc { line, col, length }` (in `lexer.h`) is attached to every
`Token` and to every `Expr` AST node:

- `Lexer` fills in `col` (1-based, within the current source line) and
  `length` (lexeme byte length).
- `ParseSignalProgram` walks line-by-line and stamps `line` on every
  token before handing them to `Parser`.
- `Parser` copies the start-of-construct token's location onto each
  produced `Expr`. Binary operators take the operator-token's location,
  conditionals take the `if` keyword's location, etc.

Errors flow through a new exception type, `ParseError`, defined in
`parser.h`. It carries a message + `SourceLoc` and renders a
compiler-style diagnostic:

```
error: Expected 'else' in conditional expression (got: '') at line 1, col 27
  | signal q = if 1 > 0 then 2
  |                           ^
```

`ParseSignalProgram` wraps any `ParseError` to attach the originating
source line; `ParseError::what()` is the full rendered diagnostic.

`CloneExpr` was updated to preserve `loc` through dependency inlining
so errors downstream of `InlineSignalDependencies` still point at the
original user source.

## P6.2 — Constant folding

`src/constant_fold.{h,cpp}` defines `FoldConstants(unique_ptr<Expr>)`,
a pure post-order rewrite that:

- Folds `UnaryOp(±)(NumberLiteral)` -> `NumberLiteral`.
- Folds `BinaryOp(arith|cmp|logical)(NumberLiteral, NumberLiteral)` ->
  `NumberLiteral`.
- Folds `Conditional(NumberLiteral, then, else)` to the picked branch.
- Folds inside `FunctionCall::args` but never folds the call itself
  (every callable in the DSL reads market state or rolling state).

`ParseSignalProgram` runs `FoldConstantsInPlace` after type-checking,
so the JIT and interpreter never see fold-able sub-expressions.

The test `constant_fold_test` includes an **IR-equality gate**: it
parses both `sma(mid(AAPL), 5 + 5)` and `sma(mid(AAPL), 10)` through
the full pipeline, compiles each with `JitCompiler::CompileProgram`,
and asserts the resulting pre-opt LLVM IR is byte-identical. If
folding ever stops reaching the IR emitter, this test fires.

## P6.3 — Type system

`src/type_check.{h,cpp}` defines `StaticType { Number, Bool }` and
walks the AST checking the rules below. The walk runs *before*
constant folding (so error spans point at user-written tokens, not at
folded literals).

Rules:

| Node                          | Operand types          | Result type |
|-------------------------------|------------------------|-------------|
| `NumberLiteral`               | -                      | `Number`    |
| `IdentifierExpr` (signal ref) | -                      | `Number`    |
| Unary `+` / `-`               | `Number`               | `Number`    |
| `+` `-` `*` `/`               | `Number`, `Number`     | `Number`    |
| `>` `<` `>=` `<=` `==` `!=`   | `Number`, `Number`     | `Bool`      |
| `&&` `||`                     | `Bool`, `Bool`         | `Bool`      |
| `if cond then a else b`       | `Bool, Number, Number` | `Number`    |
| `FunctionCall`                | (per-arg) `Number`     | `Number`    |

The signal-body rule requires `Number` (since the runtime stores
doubles). Programs that produce a `Bool` at the top level (e.g.
`signal s = mid(AAPL) > 0`) are rejected with a caret-underline error.

Examples of caught bugs:

```
signal s = if 1 then 2 else 3
                ^                  -- type error: if-condition requires `bool`

signal s = (mid(AAPL) > 0) + 1
                          ^        -- type error: arithmetic operator requires `number`
```

### Backward compatibility

Existing `.sig` files in `examples/` all pass without modification
(every conditional already uses a comparison expression). The fuzz
tests construct AST nodes directly (not through `ParseSignalProgram`)
and so are exempt from type-checking; this is intentional -- the type
checker is a source-language tool, not a runtime invariant.

---

## Build / test summary

New files:

```
src/constant_fold.{h,cpp}
src/type_check.{h,cpp}
test/source_location_test.cpp
test/constant_fold_test.cpp
test/type_check_test.cpp
```

Modified:

```
src/lexer.{h,cpp}        -- SourceLoc on Token, col tracking
src/ast.h                -- SourceLoc on every Expr
src/ast_clone.cpp        -- preserve loc through clones
src/parser.{h,cpp}       -- stamp loc on AST nodes; ParseError type
src/signal_program.cpp   -- line numbering + type-check + fold pipeline
CMakeLists.txt           -- new source files + tests
```

ctest: 27/27 passing (was 25/25 pre-P6).
