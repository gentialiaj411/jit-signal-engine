param(
  [string]$BuildDir = ".\build\Release"
)

$engine = Join-Path $BuildDir "jit_signal_engine.exe"
$jitTest = Join-Path $BuildDir "jit_test.exe"

if (-not (Test-Path $engine)) {
  $alt = Join-Path (Join-Path $BuildDir "Release") "jit_signal_engine.exe"
  if (Test-Path $alt) {
    $engine = $alt
  }
}

if (-not (Test-Path $jitTest)) {
  $altTest = Join-Path (Join-Path $BuildDir "Release") "jit_test.exe"
  if (Test-Path $altTest) {
    $jitTest = $altTest
  }
}

if (-not (Test-Path $engine)) {
  Write-Error "jit_signal_engine.exe not found under $BuildDir. Build first with: cmake --build build --config Release"
  exit 1
}

Write-Output "== CLI signal run =="
& $engine ".\examples\filtered_momentum.sig"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path $jitTest) {
  Write-Output ""
  Write-Output "== JIT/parity smoke test =="
  & $jitTest
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
  Write-Output ""
  Write-Output "WARN: jit_test.exe not found under $BuildDir; skipping parity smoke."
}

Write-Output ""
Write-Output "== Optional IR dump check =="
& $engine "--dump-ir" "--all-signals" ".\examples\filtered_momentum.sig"
if ($LASTEXITCODE -ne 0) {
  Write-Output "IR dump unavailable or failed. This is expected when LLVM/JIT is not available."
}
