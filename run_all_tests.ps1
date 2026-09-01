# run_all_tests.ps1
# Canonical Enginemon correctness verification.
#
# Delegates all process management, timeouts, exit codes, and parallel
# execution to CTest.  This script is a thin orchestration wrapper.
#
# Required external contract:
#   exit 0    = ALL PASS
#   nonzero   = any failure, timeout, or required SKIP
#
# Usage:
#   .\run_all_tests.ps1 -RomPath "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
#   .\run_all_tests.ps1 -RomPath "..." -NoBuild
#   .\run_all_tests.ps1 -RomPath "..." -Filter engine
#   .\run_all_tests.ps1 -RomPath "..." -Filter compiler
#   .\run_all_tests.ps1 -RomPath "..." -Filter corpus
#   .\run_all_tests.ps1 -RomPath "..." -Filter oracle
#   .\run_all_tests.ps1 -RomPath "..." -Filter standalone
#   .\run_all_tests.ps1 -RomPath "..." -Filter save
#
# Filter values map directly to ctest test preset names.
# "all" (default) runs every registered test.
#
# -NoBuild: skip the build step.  Fails if any required binary is missing.
#
# Key invariants checked after full run:
#   corpus lowering = 1788/1788
#   linker corpus   = 1788/1788
#   InvalidOwnership = 0
#   decoder/CFG     = PASS (corpus_test exit 0)

param(
    [Parameter(Mandatory=$true)]
    [string]$RomPath,

    [switch]$NoBuild,

    [string]$Filter = "all"
)

$ErrorActionPreference = "Continue"
$cmake     = "C:\Program Files\CMake\bin\cmake.exe"
$ctest     = "C:\Program Files\CMake\bin\ctest.exe"
$buildDir  = "build"
$testDir   = "$buildDir\tests\Release"
$toolsDir  = "$buildDir\tools\Release"
$runtimeDir= "$buildDir\runtime\Release"
$logDir    = "$buildDir\test_logs"
$expectedCorpus = "1788"

# ---------------------------------------------------------------------------
# Pre-flight: ROM
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $RomPath)) {
    Write-Host "ERROR: ROM not found: $RomPath" -ForegroundColor Red
    exit 1
}
$romAbs = (Resolve-Path -LiteralPath $RomPath).Path

if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

Write-Host ""
Write-Host "Enginemon Canonical Verification" -ForegroundColor White
Write-Host "  ROM:    $romAbs"  -ForegroundColor Gray
Write-Host "  Filter: $Filter"  -ForegroundColor Gray
Write-Host "  Date:   $(Get-Date -Format 'yyyy-MM-dd HH:mm')" -ForegroundColor Gray

# ---------------------------------------------------------------------------
# Required binaries (for -NoBuild check and Gate 1)
# ---------------------------------------------------------------------------
$requiredExes = @(
    "$testDir\runtime_test_engine.exe",
    "$testDir\runtime_test_compiler.exe",
    "$testDir\battle_test.exe",
    "$testDir\emitter_test.exe",
    "$testDir\sprite_money_test.exe",
    "$testDir\crystal_save_test.exe",
    "$testDir\golden_test.exe",
    "$testDir\legality_gate_test.exe",
    "$testDir\corpus_test.exe",
    "$testDir\linker_test.exe",
    "$testDir\compiler_integrity_test.exe",
    "$testDir\oracle_test.exe",
    "$toolsDir\corpus_lowering_audit.exe",
    "$toolsDir\profile_verify.exe",
    "$toolsDir\trainer_verify.exe",
    "$runtimeDir\enginemon_bootstrap.exe",
    "$runtimeDir\enginemon_tiles.exe",
    "$toolsDir\emon_smoke.exe",
    "$toolsDir\emon_compile.exe"
)

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Cyan
Write-Host "  Build" -ForegroundColor Cyan
Write-Host ("=" * 60) -ForegroundColor Cyan

if ($NoBuild) {
    $missing = @($requiredExes | Where-Object { -not (Test-Path $_) })
    if ($missing.Count -gt 0) {
        Write-Host "  FAIL: -NoBuild specified but required binaries are missing:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        Write-Host "  Run without -NoBuild to build." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "  SKIPPED (-NoBuild)  all required binaries present" -ForegroundColor Gray
} else {
    if (-not (Test-Path $cmake)) {
        Write-Host "  FAIL: CMake not found at $cmake" -ForegroundColor Red
        exit 1
    }

    # Configure if needed, then build via preset
    if (-not (Test-Path $buildDir)) {
        Write-Host "  Configuring..." -ForegroundColor Gray
        $env:ENGINEMON_ROM = $romAbs
        & $cmake --preset default
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  FAIL: cmake configure failed" -ForegroundColor Red
            exit 1
        }
    }

    $env:ENGINEMON_ROM = $romAbs
    $buildLog = "$logDir\build.log"
    Write-Host "  Building (preset: all)..." -ForegroundColor Gray

    $buildSw = [System.Diagnostics.Stopwatch]::StartNew()
    & $cmake --build --preset all *> $buildLog
    $buildExit = $LASTEXITCODE
    $buildSw.Stop()

    if ($buildExit -ne 0) {
        Write-Host "  FAIL: build failed in $([math]::Round($buildSw.Elapsed.TotalSeconds,1))s  (log: $buildLog)" -ForegroundColor Red
        # Show first error lines
        Get-Content $buildLog -ErrorAction SilentlyContinue |
            Where-Object { $_ -match "error C|error LNK|FAILED" } |
            Select-Object -First 8 |
            ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        exit 1
    }
    Write-Host "  PASS: build complete in $([math]::Round($buildSw.Elapsed.TotalSeconds,1))s" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Gate 1: Compile gates (exe presence + size)
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Cyan
Write-Host "  Compile Gates" -ForegroundColor Cyan
Write-Host ("=" * 60) -ForegroundColor Cyan

$compileOk = $true
foreach ($exeName in @("enginemon_bootstrap.exe", "enginemon_tiles.exe")) {
    $p = "$runtimeDir\$exeName"
    if (Test-Path $p) {
        $sz = [math]::Round((Get-Item $p).Length / 1KB)
        Write-Host "  [$(($exeName -replace '\.exe$',''))] PASS  ${sz} KB" -ForegroundColor Green
    } else {
        Write-Host "  [$(($exeName -replace '\.exe$',''))] FAIL  not found: $p" -ForegroundColor Red
        $compileOk = $false
    }
}
if (-not $compileOk) { exit 1 }

# ---------------------------------------------------------------------------
# Gate 2: CTest - all test execution delegated here
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Cyan
Write-Host "  Tests (preset: $Filter)" -ForegroundColor Cyan
Write-Host ("=" * 60) -ForegroundColor Cyan

# Ensure ROM is in environment for ctest (it reads ENGINEMON_ROM via CMake cache,
# but we also set it here in case the cache needs refreshing)
$env:ENGINEMON_ROM = $romAbs

# Re-configure with ROM path so ctest can find ROM-dependent tests
# (only needed if the cache doesn't already have it)
$cacheFile = "$buildDir\CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cached = (Get-Content $cacheFile | Select-String "ENGINEMON_ROM_PATH:STRING").Line
    if (-not $cached -or $cached -notmatch [regex]::Escape($romAbs)) {
        & $cmake -B $buildDir "-DENGINEMON_ROM_PATH=$romAbs" | Out-Null
    }
}

$ctestLog  = "$logDir\ctest_$Filter.log"
$ctestSw   = [System.Diagnostics.Stopwatch]::StartNew()

# ctest --preset handles: process launch, timeout, exit code, parallelism, labels
# --output-on-failure: only print test output when a test fails (clean terminal on pass)
# --output-log: captures CTest's own output (test names, timings, pass/fail summary)
& $ctest --preset $Filter --test-dir $buildDir --output-log $ctestLog --output-on-failure
$ctestExit = $LASTEXITCODE
$ctestSw.Stop()

Write-Host ""
Write-Host "  CTest exit: $ctestExit  ($([math]::Round($ctestSw.Elapsed.TotalSeconds,1))s)  log: $ctestLog" `
    -ForegroundColor $(if ($ctestExit -eq 0) { "Green" } else { "Yellow" })

if ($ctestExit -ne 0) {
    Write-Host "  One or more tests FAILED.  See: $ctestLog" -ForegroundColor Red
}

# ---------------------------------------------------------------------------
# Gate 3: Key invariants (parsed from ctest verbose log)
# Only checked on full runs (filter=all); irrelevant for subsystem runs.
# ---------------------------------------------------------------------------
$invariantsOk = $true
$corpusLoweringOk = $true
$linkerCorpusOk   = $true
$ownershipOk      = $true
$decoderCfgOk     = ($ctestExit -eq 0)  # corpus_test exit 0 => decoder/CFG pass

if ($Filter -eq "all") {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host "  Key Invariants" -ForegroundColor Cyan
    Write-Host ("=" * 60) -ForegroundColor Cyan

    # Run the two invariant-bearing binaries directly (they're fast: <200ms each).
    # This is simpler and more reliable than parsing ctest log output.
    $loweringOut = & "$toolsDir\corpus_lowering_audit.exe" $romAbs 2>&1
    $lm = $loweringOut | Select-String "Successes:\s+(\d+)" | Select-Object -First 1
    $loweringCount = if ($lm) { $lm.Matches[0].Groups[1].Value } else { "?" }
    $corpusLoweringOk = ($loweringCount -eq $expectedCorpus)

    $linkerOut = & "$testDir\linker_test.exe" $romAbs 2>&1
    $cm = $linkerOut | Select-String "Total unique bodies:\s+(\d+)" | Select-Object -First 1
    $linkerCount = if ($cm) { $cm.Matches[0].Groups[1].Value } else { "?" }
    $om = $linkerOut | Select-String "InvalidOwnership:\s+(\d+)" | Select-Object -First 1
    $ownershipVal = if ($om) { $om.Matches[0].Groups[1].Value } else { "?" }
    $linkerCorpusOk = ($linkerCount -eq $expectedCorpus)
    $ownershipOk    = ($ownershipVal -eq "0")

    $invariantsOk = $corpusLoweringOk -and $linkerCorpusOk -and $ownershipOk

    Write-Host "    corpus lowering  = $loweringCount/$expectedCorpus" `
        -ForegroundColor $(if ($corpusLoweringOk) { "Green" } else { "Red" })
    Write-Host "    linker corpus    = $linkerCount/$expectedCorpus" `
        -ForegroundColor $(if ($linkerCorpusOk) { "Green" } else { "Red" })
    Write-Host "    InvalidOwnership = $ownershipVal" `
        -ForegroundColor $(if ($ownershipOk) { "Green" } else { "Red" })
    Write-Host "    decoder/CFG      = $(if ($decoderCfgOk) { 'PASS' } else { 'FAIL' })" `
        -ForegroundColor $(if ($decoderCfgOk) { "Green" } else { "Red" })
}

# ---------------------------------------------------------------------------
# Gate 4: Production runtime smoke
# Two-step tool pipeline; not a ctest test.  Only runs on full suite.
# ---------------------------------------------------------------------------
$smokeOk = $true
if ($Filter -eq "all") {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host "  Production Runtime Smoke" -ForegroundColor Cyan
    Write-Host ("=" * 60) -ForegroundColor Cyan

    $smokeExe    = "$toolsDir\emon_smoke.exe"
    $compileExe  = "$toolsDir\emon_compile.exe"
    $smokePkg    = [System.IO.Path]::Combine(
        [System.IO.Path]::GetTempPath(),
        "smoke_$([System.Diagnostics.Process]::GetCurrentProcess().Id).emon"
    )

    if (-not (Test-Path $smokeExe) -or -not (Test-Path $compileExe)) {
        Write-Host "  FAIL: emon_compile or emon_smoke not found" -ForegroundColor Red
        $smokeOk = $false
    } else {
        $smokeSw = [System.Diagnostics.Stopwatch]::StartNew()
        $compileOut = & $compileExe $romAbs $smokePkg "--no-cache" 2>&1
        $compileExit = $LASTEXITCODE

        if ($compileExit -ne 0 -or -not (Test-Path $smokePkg)) {
            Write-Host "  FAIL: emon_compile failed (exit $compileExit)" -ForegroundColor Red
            $smokeOk = $false
        } else {
            $smokeOut  = & $smokeExe $smokePkg 2>&1
            $smokeExit = $LASTEXITCODE
            $smokeSw.Stop()
            $pkgKb = [math]::Round((Get-Item $smokePkg).Length / 1KB)

            if ($smokeExit -eq 0) {
                Write-Host "  PASS  fresh ROM->compile->smoke ($([math]::Round($smokeSw.Elapsed.TotalSeconds,1))s, ${pkgKb} KB)" `
                    -ForegroundColor Green
            } else {
                Write-Host "  FAIL: emon_smoke exit $smokeExit ($([math]::Round($smokeSw.Elapsed.TotalSeconds,1))s)" `
                    -ForegroundColor Red
                $smokeOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
                $smokeOk = $false
            }
        }
        Remove-Item -LiteralPath $smokePkg -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor White
Write-Host "  SUMMARY" -ForegroundColor White
Write-Host ("=" * 60) -ForegroundColor White

$allOk = ($ctestExit -eq 0) -and $invariantsOk -and $smokeOk

if ($Filter -ne "all") {
    $label = if ($ctestExit -eq 0) { "PARTIAL PASS" } else { "PARTIAL FAIL" }
    $color = if ($ctestExit -eq 0) { "Green" } else { "Red" }
    Write-Host "  $label (filter: $Filter)" -ForegroundColor $color
    Write-Host ""
    exit $ctestExit
}

if ($allOk) {
    Write-Host "  OVERALL: PASS" -ForegroundColor Green -BackgroundColor DarkGreen
} else {
    Write-Host "  OVERALL: FAIL" -ForegroundColor Red -BackgroundColor DarkRed
}
Write-Host ""
exit $(if ($allOk) { 0 } else { 1 })
