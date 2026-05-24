#include "signal_program.h"

#include <cctype>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "ast_clone.h"
#include "lexer.h"
#include "parser.h"
#include "runtime.h"

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

}  // namespace

std::vector<SignalDef> ParseSignalProgram(const std::string& source) {
  std::vector<SignalDef> out;
  std::stringstream ss(source);
  std::string line;
  while (std::getline(ss, line)) {
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
    line = Trim(line);
    if (line.empty()) continue;
    Lexer lexer(line);
    Parser parser(lexer.Tokenize());
    out.push_back(parser.ParseSignalDef());
  }
  if (out.empty()) throw std::runtime_error("No signal definitions found");
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

std::int64_t AllocateNodeIds(SignalDef& signal) {
  std::int64_t next_id = 1;
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
      const bool stateful =
          (fn->name == "ema" || fn->name == "sma" || fn->name == "rolling_std" || fn->name == "rolling_min" ||
           fn->name == "rolling_max" || fn->name == "zscore" || fn->name == "vwap" || fn->name == "lag" ||
           fn->name == "cross_above" || fn->name == "cross_below");
      if (stateful && fn->node_id < 0) {
        fn->node_id = next_id++;
      }
      for (auto& a : fn->args) {
        walk(*a);
      }
      return;
    }
  };
  walk(*signal.body);
  return next_id - 1;
}

std::int64_t AllocateProgramNodeIds(std::vector<SignalDef>& signals) {
  std::int64_t next_id = 1;
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
      const bool stateful =
          (fn->name == "ema" || fn->name == "sma" || fn->name == "rolling_std" || fn->name == "rolling_min" ||
           fn->name == "rolling_max" || fn->name == "zscore" || fn->name == "vwap" || fn->name == "lag" ||
           fn->name == "cross_above" || fn->name == "cross_below");
      if (stateful && fn->node_id < 0) {
        fn->node_id = next_id++;
      }
      for (auto& a : fn->args) {
        walk(*a);
      }
      return;
    }
  };

  for (auto& signal : signals) {
    walk(*signal.body);
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
