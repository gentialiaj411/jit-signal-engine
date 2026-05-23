#pragma once

#include <functional>
#include <memory>
#include <string>

#include "ast.h"
#include "runtime.h"

namespace jitse {

class JitCompiler {
 public:
  using JitFn = double (*)(const MarketState*, SignalContext*);
  using ProgramFn = void (*)(const MarketState*, SignalContext*, double*);

  JitCompiler();
  ~JitCompiler();

  JitCompiler(const JitCompiler&) = delete;
  JitCompiler& operator=(const JitCompiler&) = delete;

  bool IsAvailable() const;
  bool HasAVX2() const;
  std::string LastError() const;

  // Compile one signal expression into native code.
  bool Compile(const SignalDef& signal, const SymbolTable& symbols);
  bool CompileProgram(const std::vector<SignalDef>& signals, const SymbolTable& symbols);
  void DumpLastIR() const;
  void DumpLastIRPreOpt() const;

  JitFn GetFunction() const;
  ProgramFn GetProgramFunction() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jitse
