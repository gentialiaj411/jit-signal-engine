#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "runtime.h"

namespace jitse {

using CompiledSignalFn = double (*)(const MarketState*, SignalContext*);

class CompiledSignal {
 public:
  virtual ~CompiledSignal() = default;
  virtual bool IsAvailable() const = 0;
  virtual bool Compile(const SignalDef& signal, const SymbolTable& symbols) = 0;
  virtual CompiledSignalFn GetFunction() const = 0;
  virtual void DumpLastIR() const = 0;
  virtual void DumpLastIRPreOpt() const = 0;
  virtual std::string LastError() const = 0;
  virtual const char* BackendName() const = 0;
};

std::unique_ptr<CompiledSignal> CreateLlvmBackend();

}  // namespace jitse
