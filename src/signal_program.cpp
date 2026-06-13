#include "signal_program.h"

#include <cctype>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "ast_clone.h"  // AstEquals for P11 stateful-subtree dedup
#include "constant_fold.h"
#include "lexer.h"
#include "parser.h"
#include "runtime.h"
#include "type_check.h"

namespace jitse {

namespace {

std::string Trim(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  std::size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::unique_ptr<Expr> InlineExpr(const Expr& expr, const std::unordered_map<std::string, const Expr*>& defs) {
  if (const auto* n = dynamic_cast<const NumberLiteral*>(&expr)) {
    return std::make_unique<NumberLiteral>(n->value);
  }
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    auto it = defs.find(id->name);
    if (it != defs.end()) {
      return CloneExpr(*it->second);
    }
    return std::make_unique<IdentifierExpr>(id->name);
  }
  if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
    return std::make_unique<ParameterExpr>(p->name, p->param_id);
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    return std::make_unique<UnaryOp>(u->kind, InlineExpr(*u->operand, defs));
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    return std::make_unique<BinaryOp>(b->kind, InlineExpr(*b->left, defs), InlineExpr(*b->right, defs));
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    return std::make_unique<Conditional>(
        InlineExpr(*c->condition, defs), InlineExpr(*c->then_branch, defs), InlineExpr(*c->else_branch, defs));
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    std::vector<std::unique_ptr<Expr>> args;
    args.reserve(fn->args.size());
    for (const auto& a : fn->args) args.push_back(InlineExpr(*a, defs));
    return std::make_unique<FunctionCall>(fn->name, std::move(args));
  }
  throw std::runtime_error("InlineExpr: unknown AST node");
}

void CollectRefs(const Expr& expr, std::unordered_set<std::string>& refs) {
  if (const auto* id = dynamic_cast<const IdentifierExpr*>(&expr)) {
    refs.insert(id->name);
    return;
  }
  if (const auto* p = dynamic_cast<const ParameterExpr*>(&expr)) {
    (void)p;
    return;
  }
  if (const auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
    CollectRefs(*u->operand, refs);
    return;
  }
  if (const auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
    CollectRefs(*b->left, refs);
    CollectRefs(*b->right, refs);
    return;
  }
  if (const auto* c = dynamic_cast<const Conditional*>(&expr)) {
    CollectRefs(*c->condition, refs);
    CollectRefs(*c->then_branch, refs);
    CollectRefs(*c->else_branch, refs);
    return;
  }
  if (const auto* fn = dynamic_cast<const FunctionCall*>(&expr)) {
    // Function arg 0 for market-data funcs is ticker, not signal ref.
    const bool ticker_first_arg =
        (fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread" || fn->name == "vwap");
    for (std::size_t i = 0; i < fn->args.size(); ++i) {
      if (ticker_first_arg && i == 0) continue;
      CollectRefs(*fn->args[i], refs);
    }
    return;
  }
}

void ResolveParameterRefs(std::unique_ptr<Expr>& expr, const std::unordered_map<std::string, std::int64_t>& param_ids) {
  if (auto* id = dynamic_cast<IdentifierExpr*>(expr.get())) {
    auto it = param_ids.find(id->name);
    if (it != param_ids.end()) {
      auto param = std::make_unique<ParameterExpr>(id->name, it->second);
      param->loc = id->loc;
      expr = std::move(param);
    }
    return;
  }
  if (dynamic_cast<ParameterExpr*>(expr.get()) != nullptr) {
    return;
  }
  if (auto* u = dynamic_cast<UnaryOp*>(expr.get())) {
    ResolveParameterRefs(u->operand, param_ids);
    return;
  }
  if (auto* b = dynamic_cast<BinaryOp*>(expr.get())) {
    ResolveParameterRefs(b->left, param_ids);
    ResolveParameterRefs(b->right, param_ids);
    return;
  }
  if (auto* c = dynamic_cast<Conditional*>(expr.get())) {
    ResolveParameterRefs(c->condition, param_ids);
    ResolveParameterRefs(c->then_branch, param_ids);
    ResolveParameterRefs(c->else_branch, param_ids);
    return;
  }
  if (auto* fn = dynamic_cast<FunctionCall*>(expr.get())) {
    const bool ticker_first_arg =
        (fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread" || fn->name == "vwap");
    for (std::size_t i = 0; i < fn->args.size(); ++i) {
      if (ticker_first_arg && i == 0) continue;
      ResolveParameterRefs(fn->args[i], param_ids);
    }
    return;
  }
}

}  // namespace

ProgramDef ParseProgram(const std::string& source) {
  // P6.1: track 1-based source line numbers as we tokenize and stamp
  // each Token's `loc.line` before passing to the parser. If a parse
  // failure escapes the parser, attach the original line text so the
  // error renderer can draw a caret.
  ProgramDef out;
  std::stringstream ss(source);
  std::string raw_line;
  std::uint32_t line_no = 0;
  while (std::getline(ss, raw_line)) {
    ++line_no;
    std::string line = raw_line;
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
    line = Trim(line);
    if (line.empty()) continue;
    Lexer lexer(line);
    std::vector<Token> tokens = lexer.Tokenize();
    for (auto& t : tokens) t.loc.line = line_no;
    const TokenKind first_kind =
        tokens.empty() ? TokenKind::EndOfFile : tokens[0].kind;
    Parser parser(std::move(tokens));
    try {
      if (first_kind == TokenKind::Param) {
        ParamDef def = parser.ParseParamDef();
        if (def.param_id < 0) {
          def.param_id = static_cast<std::int64_t>(out.params.size());
        }
        out.params.push_back(std::move(def));
      } else {
        SignalDef def = parser.ParseSignalDef();
        TypeCheckSignal(def);
        FoldConstantsInPlace(def);
        out.signals.push_back(std::move(def));
      }
    } catch (const ParseError& e) {
      throw ParseError(e.Message(), e.Loc(), line);
    }
  }
  if (out.signals.empty()) throw std::runtime_error("No signal definitions found");

  std::unordered_map<std::string, std::int64_t> param_ids;
  for (std::size_t i = 0; i < out.params.size(); ++i) {
    ParamDef& p = out.params[i];
    p.param_id = static_cast<std::int64_t>(i);
    if (!param_ids.emplace(p.name, p.param_id).second) {
      throw ParseError("Duplicate parameter name: " + p.name, p.loc);
    }
  }
  std::unordered_set<std::string> signal_names;
  for (const auto& signal : out.signals) {
    if (!signal_names.emplace(signal.name).second) {
      throw std::runtime_error("Duplicate signal name: " + signal.name);
    }
    if (param_ids.find(signal.name) != param_ids.end()) {
      throw std::runtime_error("Name used by both parameter and signal: " + signal.name);
    }
  }
  for (auto& signal : out.signals) {
    ResolveParameterRefs(signal.body, param_ids);
  }
  return out;
}

std::vector<SignalDef> ParseSignalProgram(const std::string& source) {
  return ParseProgram(source).signals;
}

ProgramDef InlineSignalDependencies(const ProgramDef& program) {
  ProgramDef out;
  out.params = program.params;
  out.signals = InlineSignalDependencies(program.signals);
  return out;
}

std::vector<SignalDef> InlineSignalDependencies(const std::vector<SignalDef>& signals) {
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < signals.size(); ++i) {
    if (!index.emplace(signals[i].name, i).second) {
      throw std::runtime_error("Duplicate signal name: " + signals[i].name);
    }
  }

  std::vector<SignalDef> resolved(signals.size());
  std::vector<int> state(signals.size(), 0);  // 0 unvisited, 1 visiting, 2 done

  std::function<void(std::size_t)> dfs = [&](std::size_t i) {
    if (state[i] == 2) return;
    if (state[i] == 1) throw std::runtime_error("Cycle in signal dependencies at: " + signals[i].name);
    state[i] = 1;

    std::unordered_set<std::string> refs;
    CollectRefs(*signals[i].body, refs);
    for (const auto& r : refs) {
      auto it = index.find(r);
      if (it != index.end()) dfs(it->second);
    }

    std::unordered_map<std::string, const Expr*> defs;
    for (const auto& r : refs) {
      auto it = index.find(r);
      if (it != index.end()) defs.emplace(r, resolved[it->second].body.get());
    }
    resolved[i].name = signals[i].name;
    resolved[i].body = InlineExpr(*signals[i].body, defs);
    state[i] = 2;
  };

  for (std::size_t i = 0; i < signals.size(); ++i) dfs(i);
  return resolved;
}

namespace {

bool IsStatefulOp(const std::string& name) {
  return name == "ema" || name == "ema_alpha" || name == "sma" || name == "rolling_std" || name == "rolling_min" ||
         name == "rolling_max" || name == "zscore" || name == "vwap" || name == "lag" ||
         name == "cross_above" || name == "cross_below" ||
         name == "rolling_corr" || name == "rolling_beta" || name == "kalman1d";
}

// P11: per-signal-body state for the node-id dedup pass.
//
// `seen` is the list of stateful FunctionCalls already given an id
// in this body, with a flag for whether the FIRST occurrence was at
// an unconditional position (top-level or inside a Conditional's
// `cond` -- both evaluate unconditionally every tick). A later
// structurally-equal call aliases its node-id to that first call
// only when the first was unconditional, because:
//
//   * The interpreter and JIT both run the condition before the
//     then/else branches, so the unconditional emit ALWAYS happens
//     before any later use. (For the JIT: the IR value produced by
//     the unconditional call lives in a BasicBlock that dominates
//     the conditional branches, so reusing it is valid SSA.)
//   * If the first occurrence is inside a conditional branch (then
//     or else), aliasing a later occurrence (in the OTHER branch,
//     say) to it would be wrong on both counts -- neither branch
//     dominates the other, and at runtime exactly one branch
//     executes per tick, so state pushes would silently desync.
//
// In the "aliased" case we leave the AST node in place but reuse
// its node-id; downstream the JIT's per-program stateful-emit cache
// and the interpreter's per-Evaluate cache turn that into a single
// runtime call per tick.
struct SeenStateful {
  const FunctionCall* node;
  bool unconditional;
};

// Look for a structurally-equal call in `seen`. Returns its node-id
// (>= 1) if it can be aliased; returns 0 otherwise. We only alias
// against unconditional first-occurrences, so the JIT's SSA
// dominance and the interpreter's tick-evaluation order both stay
// well-defined.
std::int64_t TryAliasNodeId(const std::vector<SeenStateful>& seen, const FunctionCall* candidate) {
  for (const auto& s : seen) {
    if (!s.unconditional) continue;
    if (s.node->name != candidate->name) continue;
    if (s.node->args.size() != candidate->args.size()) continue;
    if (AstEquals(*s.node, *candidate)) {
      return s.node->node_id;
    }
  }
  return 0;
}

}  // namespace

std::int64_t AllocateNodeIds(SignalDef& signal) {
  std::int64_t next_id = 1;
  std::vector<SeenStateful> seen;
  std::function<void(Expr&, bool)> walk = [&](Expr& expr, bool is_unconditional) {
    if (auto* u = dynamic_cast<UnaryOp*>(&expr)) {
      walk(*u->operand, is_unconditional);
      return;
    }
    if (auto* b = dynamic_cast<BinaryOp*>(&expr)) {
      walk(*b->left, is_unconditional);
      walk(*b->right, is_unconditional);
      return;
    }
    if (auto* c = dynamic_cast<Conditional*>(&expr)) {
      // The condition is itself always evaluated. The two branches
      // are NOT (one is taken per tick), so anything inside them is
      // conditional.
      walk(*c->condition, is_unconditional);
      walk(*c->then_branch, /*is_unconditional=*/false);
      walk(*c->else_branch, /*is_unconditional=*/false);
      return;
    }
    if (auto* fn = dynamic_cast<FunctionCall*>(&expr)) {
      const bool stateful = IsStatefulOp(fn->name);
      if (stateful && fn->node_id < 0) {
        const std::int64_t aliased = TryAliasNodeId(seen, fn);
        if (aliased > 0) {
          fn->node_id = aliased;
        } else {
          fn->node_id = next_id++;
          seen.push_back(SeenStateful{fn, is_unconditional});
        }
      }
      for (auto& a : fn->args) {
        walk(*a, is_unconditional);
      }
      return;
    }
  };
  walk(*signal.body, /*is_unconditional=*/true);
  return next_id - 1;
}

std::int64_t AllocateProgramNodeIds(std::vector<SignalDef>& signals) {
  std::int64_t next_id = 1;
  // Per-body `seen` list (reset between signals). We do NOT dedup
  // across signals because the interpreter's per-Evaluate cache and
  // the JIT's signal_values cache only memoize within one signal
  // body, so sharing a node-id ACROSS signals would push the
  // operator's state twice per tick (once per signal-Evaluate call).
  std::vector<SeenStateful> seen;
  std::function<void(Expr&, bool)> walk = [&](Expr& expr, bool is_unconditional) {
    if (auto* u = dynamic_cast<UnaryOp*>(&expr)) {
      walk(*u->operand, is_unconditional);
      return;
    }
    if (auto* b = dynamic_cast<BinaryOp*>(&expr)) {
      walk(*b->left, is_unconditional);
      walk(*b->right, is_unconditional);
      return;
    }
    if (auto* c = dynamic_cast<Conditional*>(&expr)) {
      walk(*c->condition, is_unconditional);
      walk(*c->then_branch, /*is_unconditional=*/false);
      walk(*c->else_branch, /*is_unconditional=*/false);
      return;
    }
    if (auto* fn = dynamic_cast<FunctionCall*>(&expr)) {
      const bool stateful = IsStatefulOp(fn->name);
      if (stateful && fn->node_id < 0) {
        const std::int64_t aliased = TryAliasNodeId(seen, fn);
        if (aliased > 0) {
          fn->node_id = aliased;
        } else {
          fn->node_id = next_id++;
          seen.push_back(SeenStateful{fn, is_unconditional});
        }
      }
      for (auto& a : fn->args) {
        walk(*a, is_unconditional);
      }
      return;
    }
  };

  for (auto& signal : signals) {
    seen.clear();
    walk(*signal.body, /*is_unconditional=*/true);
  }
  return next_id - 1;
}

void BindSymbolIds(SignalDef& signal, const SymbolTable& symbols) {
  std::function<void(Expr&)> walk = [&](Expr& expr) {
    if (auto* u = dynamic_cast<UnaryOp*>(&expr)) {
      walk(*u->operand);
      return;
    }
    if (auto* b = dynamic_cast<BinaryOp*>(&expr)) {
      walk(*b->left);
      walk(*b->right);
      return;
    }
    if (auto* c = dynamic_cast<Conditional*>(&expr)) {
      walk(*c->condition);
      walk(*c->then_branch);
      walk(*c->else_branch);
      return;
    }
    if (auto* fn = dynamic_cast<FunctionCall*>(&expr)) {
      const bool has_ticker_arg =
          (fn->name == "mid" || fn->name == "bid" || fn->name == "ask" || fn->name == "spread" || fn->name == "vwap");
      if (has_ticker_arg && !fn->args.empty()) {
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(fn->args[0].get())) {
          fn->symbol_id = static_cast<std::int64_t>(symbols.LookupId(id->name));
        } else {
          throw std::runtime_error(fn->name + "() first argument must be ticker identifier");
        }
      }
      for (auto& a : fn->args) {
        walk(*a);
      }
      return;
    }
  };
  walk(*signal.body);
}

}  // namespace jitse
