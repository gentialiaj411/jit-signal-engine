param(
  [string]$BuildDir = ".\build\Debug",
  [string]$OutCsv = ".\bench\results.csv"
)

if (Test-Path $OutCsv) {
  Remove-Item -LiteralPath $OutCsv -Force
}

$bench = Join-Path $BuildDir "signal_benchmark.exe"

& $bench ".\examples\spread_signal.sig" 50000 $OutCsv
& $bench ".\examples\momentum_signal.sig" 50000 $OutCsv
& $bench ".\examples\zscore_signal.sig" 50000 $OutCsv
& $bench ".\examples\filtered_momentum.sig" 50000 $OutCsv filtered
& $bench ".\examples\vwap_signal.sig" 50000 $OutCsv dev
& $bench ".\examples\zscore_builtin_signal.sig" 50000 $OutCsv z

Write-Output "Wrote benchmark csv: $OutCsv"
