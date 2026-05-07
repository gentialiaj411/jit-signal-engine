#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.h"

namespace jitse {

std::vector<SignalDef> ParseSignalProgram(const std::string& source);
std::vector<SignalDef> InlineSignalDependencies(const std::vector<SignalDef>& signals);
std::int64_t AllocateNodeIds(SignalDef& signal);

}  // namespace jitse
