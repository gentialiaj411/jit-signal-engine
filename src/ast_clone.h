#pragma once

#include <memory>

#include "ast.h"

namespace jitse {

std::unique_ptr<Expr> CloneExpr(const Expr& expr);

}  // namespace jitse

