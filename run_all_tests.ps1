# run_all_tests.ps1
# Canonical Enginemon verification script
# Runs all test suites and reports unified pass/fail status
#
# Required invariants:
#   - corpus = 1679/1679
#   - InvalidOwnership = 0
#   - All test suites pass
#
# Exit codes:
#   0 = ALL PASS
#   1 = FAILURE (details reported)

param(
    [Parameter(Mandatory=$true)]
    [string]$RomPath
)

$ErrorActionPreference = "Stop"

# Configuration
$buildDir = "build"
$releaseDir = "$buildDir\tests\Release"

# Counters
$passed = 0
$failed = 0
$failedTests = @()

function Write-TestHeader($name) {
    Write-Host "`n" -NoNewline
    Write-Host "=" * 60 -ForegroundColor Cyan
    Write-Host "  $name" -ForegroundColor Cyan
    Write-Host "=" * 60 -ForegroundColor Cyan
}

function Write-TestResult($name, $success, $details = "") {
    if ($success) {
        Write-Host "  [$name] " -NoNewline
        Write-Host "PASS" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  [$name] " -NoNewline
        Write-Host "FAIL" -ForegroundColor Red
        if ($details) {
            Write-Host "    $details" -ForegroundColor Yellow
        }
        $script:failed++
        $script:failedTests += $name
    }
}

# Verify ROM exists
if (-not (Test-Path $RomPath)) {
    Write-Host "ERROR: ROM not found at: $RomPath" -ForegroundColor Red
    exit 1
}

# Verify build exists
if (-not (Test-Path $releaseDir)) {
    Write-Host "ERROR: Build not found at: $releaseDir" -ForegroundColor Red
    Write-Host "Run the following to build:" -ForegroundColor Yellow
    Write-Host '  & "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release' -ForegroundColor Yellow
    exit 1
}

Write-Host "`nEngineemon Canonical Verification" -ForegroundColor White
Write-Host "ROM: $RomPath" -ForegroundColor Gray
Write-Host "Build: $releaseDir" -ForegroundColor Gray

# =============================================================================
# Suite 1: Runtime Tests (232 tests)
# =============================================================================
Write-TestHeader "Runtime Tests"

$runtimeExe = "$releaseDir\runtime_test.exe"
if (Test-Path $runtimeExe) {
    $output = & $runtimeExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    
    # Parse output for pass/fail count
    $passMatch = $output | Select-String -Pattern "(\d+)/(\d+) tests passed"
    if ($passMatch) {
        $passCount = $passMatch.Matches[0].Groups[1].Value
        $totalCount = $passMatch.Matches[0].Groups[2].Value
        Write-TestResult "runtime_test" ($exitCode -eq 0 -and $passCount -eq $totalCount) "$passCount/$totalCount"
    } else {
        Write-TestResult "runtime_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "runtime_test" $false "Executable not found"
}

# =============================================================================
# Suite 2: Golden Tests (56 tests)
# =============================================================================
Write-TestHeader "Golden Tests"

$goldenExe = "$releaseDir\golden_test.exe"
if (Test-Path $goldenExe) {
    $output = & $goldenExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    
    $passMatch = $output | Select-String -Pattern "(\d+)/(\d+) tests passed"
    if ($passMatch) {
        $passCount = $passMatch.Matches[0].Groups[1].Value
        $totalCount = $passMatch.Matches[0].Groups[2].Value
        Write-TestResult "golden_test" ($exitCode -eq 0 -and $passCount -eq $totalCount) "$passCount/$totalCount"
    } else {
        Write-TestResult "golden_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "golden_test" $false "Executable not found"
}

# =============================================================================
# Suite 3: Legality Gate Tests (14 adversarial tests)
# =============================================================================
Write-TestHeader "Legality Gate Tests"

$legalityExe = "$releaseDir\legality_gate_test.exe"
if (Test-Path $legalityExe) {
    $output = & $legalityExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    
    $passMatch = $output | Select-String -Pattern "(\d+)/(\d+) tests passed"
    if ($passMatch) {
        $passCount = $passMatch.Matches[0].Groups[1].Value
        $totalCount = $passMatch.Matches[0].Groups[2].Value
        Write-TestResult "legality_gate_test" ($exitCode -eq 0 -and $passCount -eq $totalCount) "$passCount/$totalCount"
    } else {
        Write-TestResult "legality_gate_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "legality_gate_test" $false "Executable not found"
}

# =============================================================================
# Suite 4: Corpus Test (Decoder/CFG Integrity)
# Stage 1: Typed decoding validation
# Stage 2: CFG construction and validation  
# Stage 3: NativeCallRegistry + RamAddressRegistry classification
# =============================================================================
Write-TestHeader "Corpus Test (Decoder/CFG Integrity)"

$corpusExe = "$releaseDir\corpus_test.exe"
if (Test-Path $corpusExe) {
    $output = & $corpusExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    
    # Check for decoder failures
    $decoderOk = -not ($output -match "round.?trip.?fail|decode.?fail|unknown.?opcode")
    
    Write-TestResult "corpus_test" ($exitCode -eq 0 -and $decoderOk) "Exit code: $exitCode"
} else {
    Write-TestResult "corpus_test" $false "Executable not found"
}

# =============================================================================
# Suite 5: Linker Test (corpus=1679, InvalidOwnership=0)
# =============================================================================
Write-TestHeader "Linker Test"

$linkerExe = "$releaseDir\linker_test.exe"
$corpusOk = $false
$ownershipOk = $false
$corpusCount = "?"

if (Test-Path $linkerExe) {
    $output = & $linkerExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    
    # Check corpus count
    $corpusMatch = $output | Select-String -Pattern "Total unique bodies:\s*(\d+)"
    if ($corpusMatch) {
        $corpusCount = $corpusMatch.Matches[0].Groups[1].Value
        $corpusOk = ($corpusCount -eq "1679")
    }
    
    # Check InvalidOwnership count
    $ownerMatch = $output | Select-String -Pattern "InvalidOwnership:\s*(\d+)"
    if ($ownerMatch) {
        $ownerCount = $ownerMatch.Matches[0].Groups[1].Value
        $ownershipOk = ($ownerCount -eq "0")
    } else {
        # If not explicitly mentioned, check for absence of ownership errors
        $ownershipOk = -not ($output -match "InvalidOwnership|ownership.?error|invalid.?owner")
    }
    
    $linkerPass = ($exitCode -eq 0) -and $corpusOk -and $ownershipOk
    Write-TestResult "linker_test" $linkerPass "corpus=$corpusCount/1679, InvalidOwnership=$($ownershipOk ? 0 : '?')"
} else {
    Write-TestResult "linker_test" $false "Executable not found"
}

# =============================================================================
# Suite 6: StdScripts Coverage (Stage 5)
# =============================================================================
Write-TestHeader "StdScripts Coverage Test"

$stdscriptsExe = "$releaseDir\stdscripts_coverage_test.exe"
if (Test-Path $stdscriptsExe) {
    $output = & $stdscriptsExe $RomPath 2>&1
    $exitCode = $LASTEXITCODE
    Write-TestResult "stdscripts_coverage_test" ($exitCode -eq 0) "Exit code: $exitCode"
} else {
    Write-TestResult "stdscripts_coverage_test" $false "Executable not found"
}

# =============================================================================
# Summary
# =============================================================================
Write-Host "`n" -NoNewline
Write-Host "=" * 60 -ForegroundColor White
Write-Host "  SUMMARY" -ForegroundColor White
Write-Host "=" * 60 -ForegroundColor White

Write-Host "`n  Passed: $passed" -ForegroundColor Green
Write-Host "  Failed: $failed" -ForegroundColor $(if ($failed -gt 0) { "Red" } else { "Green" })

if ($failed -gt 0) {
    Write-Host "`n  Failed tests:" -ForegroundColor Red
    foreach ($test in $failedTests) {
        Write-Host "    - $test" -ForegroundColor Red
    }
}

# Key invariants
Write-Host "`n  Key Invariants:" -ForegroundColor Gray
Write-Host "    corpus = $corpusCount/1679" -ForegroundColor $(if ($corpusOk) { "Green" } else { "Red" })
Write-Host "    InvalidOwnership = $($ownershipOk ? '0' : 'ERROR')" -ForegroundColor $(if ($ownershipOk) { "Green" } else { "Red" })

# Final result
Write-Host "`n" -NoNewline
if ($failed -eq 0 -and $corpusOk -and $ownershipOk) {
    Write-Host "  OVERALL: PASS" -ForegroundColor Green -BackgroundColor DarkGreen
    Write-Host ""
    exit 0
} else {
    Write-Host "  OVERALL: FAIL" -ForegroundColor Red -BackgroundColor DarkRed
    Write-Host ""
    exit 1
}
