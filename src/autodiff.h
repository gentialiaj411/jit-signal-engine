#pragma once

#include <cstdint>
#include <vector>

#include "ast.h"
#include "runtime.h"

namespace jitse {

struct ValueGradient {
  double value = 0.0;
  double gradient = 0.0;
};

// Builds one stateless gradient signal per parameter for `signal`.
//
// Scope is intentionally Phase-1 narrow:
//   - arithmetic, unary +/-,
//   - conditionals with stop-gradient through predicates,
//   - comparisons/logicals as zero-gradient,
//   - market reads as constants w.r.t. params,
//   - abs/log/sqrt.
//
// Stateful operators are rejected here and land in Phase 2.
std::vector<SignalDef> BuildStatelessGradientSignals(
    const SignalDef& signal,
    const std::vector<ParamDef>& params);

// Phase 2 forward-sensitivity evaluator. Reuses the existing SignalContext
// state model and carries one sensitivity state per (node_id, param_id).
ValueGradient EvaluateSignalGradient(
    const SignalDef& signal,
    const SymbolTable& symbols,
    const MarketState& market,
    SignalContext& ctx,
    std::int64_t param_id);

}  // namespace jitse
