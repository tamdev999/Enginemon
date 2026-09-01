# build_all.ps1
# Thin wrapper around cmake --build --preset.
# All build configuration lives in CMakePresets.json.
#
# Usage:
#   .\build_all.ps1                     # build all canonical targets
#   .\build_all.ps1 -Preset engine      # build engine targets only
#   .\build_all.ps1 -Preset compiler
#   .\build_all.ps1 -Preset corpus
#   .\build_all.ps1 -Preset oracle
#   .\build_all.ps1 -Preset smoke
#   .\build_all.ps1 -Preset save
#   .\build_all.ps1 -Configure          # (re)configure then build all
#   .\build_all.ps1 -Clean              # clean then build all
#
# Exit codes:  0 = success   1 = failure

param(
    [string]$Preset    = "all",
    [switch]$Configure,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $cmake)) {
    Write-Error "CMake not found at $cmake"
    exit 1
}

# Configure if no build dir or explicitly requested
if ($Configure -or -not (Test-Path "build")) {
    Write-Host "Configuring (preset: default)..." -ForegroundColor Cyan
    & $cmake --preset default
    if ($LASTEXITCODE -ne 0) { Write-Error "Configure failed"; exit 1 }
}

# Clean if requested
if ($Clean) {
    Write-Host "Cleaning..." -ForegroundColor Cyan
    & $cmake --build --preset $Preset --target clean
}

# Build
Write-Host "Building (preset: $Preset)..." -ForegroundColor Cyan
$t = [System.Diagnostics.Stopwatch]::StartNew()
& $cmake --build --preset $Preset
$exit = $LASTEXITCODE
$t.Stop()

if ($exit -ne 0) {
    Write-Host "Build FAILED in $([math]::Round($t.Elapsed.TotalSeconds,1))s" -ForegroundColor Red
    exit 1
}
Write-Host "Build OK in $([math]::Round($t.Elapsed.TotalSeconds,1))s" -ForegroundColor Green
exit 0
