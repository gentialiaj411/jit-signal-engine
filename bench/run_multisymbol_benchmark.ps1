param(
  [string]$BuildDir = ".\build\Release",
  [string]$OutCsv = ".\bench\results_multisymbol.csv",
  [long]$Passes = 2000
)

$bench = Join-Path $BuildDir "multisymbol_benchmark.exe"
if (-not (Test-Path $bench)) {
  Write-Error "Benchmark binary not found at $bench"
  exit 1
}

& $bench $Passes $OutCsv
Write-Host "Results written to: $OutCsv"
