# Run all signal benchmarks in Release mode and write results to CSV.
# Usage: .\bench\run_benchmarks.ps1 [-BuildDir .\build\Release] [-OutCsv .\bench\results.csv] [-PinCore 0] [-Events 1000000]
param(
  [string]$BuildDir = ".\build\Release",
  [string]$OutCsv = ".\bench\results.csv",
  [int]$PinCore = 0,
  [int]$Events = 1000000
)

$bench = Join-Path $BuildDir "signal_benchmark.exe"
if (-not (Test-Path $bench)) {
  Write-Error "Benchmark binary not found: $bench. Build with: cmake --build build --config Release"
  exit 1
}

if (Test-Path $OutCsv) {
  Remove-Item -LiteralPath $OutCsv -Force
}

$pin = "--pin-core", $PinCore

# Single-signal runs (evaluate only the last/named signal).
& $bench @pin ".\examples\spread_signal.sig"          $Events $OutCsv spread
& $bench @pin ".\examples\momentum_signal.sig"         $Events $OutCsv momentum
& $bench @pin ".\examples\zscore_signal.sig"           $Events $OutCsv spread_z
& $bench @pin ".\examples\zscore_builtin_signal.sig"   $Events $OutCsv z
& $bench @pin ".\examples\vwap_signal.sig"             $Events $OutCsv dev

# All-signals run (CompileProgram / eval_all path — the CSE/fusion path).
& $bench @pin --all-signals ".\examples\filtered_momentum.sig" $Events $OutCsv

Write-Output ""
Write-Output "Results written to: $OutCsv"
Write-Output "Verify jit_mode=enabled for JIT numbers to be meaningful."
