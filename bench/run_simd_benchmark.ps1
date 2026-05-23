param(
  [string]$BuildDir = ".\build\Release",
  [string]$OutCsv = ".\bench\results_simd.csv",
  [long]$Events = 1000000
)

$bench = Join-Path $BuildDir "simd_benchmark.exe"
if (-not (Test-Path $bench)) {
  Write-Error "Benchmark binary not found at $bench"
  exit 1
}

& $bench $Events $OutCsv
Write-Host "Results written to: $OutCsv"
