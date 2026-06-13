#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.h"

namespace jitse {
class SymbolTable;

ProgramDef ParseProgram(const std::string& source);
std::vector<SignalDef> ParseSignalProgram(const std::string& source);
ProgramDef InlineSignalDependencies(const ProgramDef& program);
std::vector<SignalDef> InlineSignalDependencies(const std::vector<SignalDef>& signals);
std::int64_t AllocateNodeIds(SignalDef& signal);
std::int64_t AllocateProgramNodeIds(std::vector<SignalDef>& signals);
void BindSymbolIds(SignalDef& signal, const SymbolTable& symbols);

}  // namespace jitse
