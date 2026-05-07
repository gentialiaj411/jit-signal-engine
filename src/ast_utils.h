#pragma once

#include <string>
#include <unordered_set>

#include "ast.h"

namespace jitse {

std::unordered_set<std::string> CollectTickerSymbols(const SignalDef& signal);

}  // namespace jitse

