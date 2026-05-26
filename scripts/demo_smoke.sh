#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-./build}"
ENGINE="$BUILD_DIR/jit_signal_engine"
JIT_TEST="$BUILD_DIR/jit_test"

if [[ ! -x "$ENGINE" && -x "$BUILD_DIR/Release/jit_signal_engine" ]]; then
  ENGINE="$BUILD_DIR/Release/jit_signal_engine"
fi
if [[ ! -x "$JIT_TEST" && -x "$BUILD_DIR/Release/jit_test" ]]; then
  JIT_TEST="$BUILD_DIR/Release/jit_test"
fi

if [[ ! -x "$ENGINE" ]]; then
  echo "ERROR: jit_signal_engine not found under $BUILD_DIR"
  echo "Build first: cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR"
  exit 1
fi

echo "== CLI signal run =="
"$ENGINE" examples/filtered_momentum.sig

if [[ -x "$JIT_TEST" ]]; then
  echo ""
  echo "== JIT/parity smoke test =="
  "$JIT_TEST"
else
  echo ""
  echo "WARN: jit_test not found under $BUILD_DIR; skipping parity smoke."
fi

echo ""
echo "== Optional IR dump check =="
if "$ENGINE" --dump-ir --all-signals examples/filtered_momentum.sig >/tmp/jitse_demo_ir.txt 2>/tmp/jitse_demo_ir.err; then
  echo "IR dump succeeded. First lines:"
  sed -n '1,20p' /tmp/jitse_demo_ir.txt
else
  echo "IR dump unavailable or failed. This is expected when LLVM/JIT is not available."
  sed -n '1,20p' /tmp/jitse_demo_ir.err || true
fi
