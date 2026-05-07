#include "signal_backend.h"

#include <memory>

#include "jit_compiler.h"

namespace jitse {

namespace {

class LlvmCompiledSignal final : public CompiledSignal {
 public:
  bool IsAvailable() const override { return jit_.IsAvailable(); }
  bool Compile(const SignalDef& signal, const SymbolTable& symbols) override {
    return jit_.Compile(signal, symbols);
  }
  CompiledSignalFn GetFunction() const override { return jit_.GetFunction(); }
  void DumpLastIR() const override { jit_.DumpLastIR(); }
  void DumpLastIRPreOpt() const override { jit_.DumpLastIRPreOpt(); }
  std::string LastError() const override { return jit_.LastError(); }
  const char* BackendName() const override { return "llvm_orc"; }

 private:
  JitCompiler jit_;
};

}  // namespace

std::unique_ptr<CompiledSignal> CreateLlvmBackend() { return std::make_unique<LlvmCompiledSignal>(); }

}  // namespace jitse
