# run_all_tests.ps1
# Canonical Enginemon verification script
# Runs all test suites and reports unified pass/fail status
#
# Required invariants:
#   - corpus lowering = 1788/1788
#   - linker corpus   = 1788/1788
#   - InvalidOwnership = 0
#   - decoder/CFG integrity = PASS
#   - All test suites pass
#
# Exit codes:
#   0 = ALL PASS
#   1 = FAILURE (details reported)
#
# Usage:
#   .\run_all_tests.ps1 -RomPath "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"

param(
    [Parameter(Mandatory=$true)]
    [string]$RomPath
)

# Use Continue for ErrorActionPreference to prevent stderr from native commands
# from being treated as terminating errors. We determine pass/fail from exit code.
$ErrorActionPreference = "Continue"

# Configuration
$buildDir = "build"
$testReleaseDir = "$buildDir\tests\Release"
$toolsReleaseDir = "$buildDir\tools\Release"

# Expected corpus count - updated when new script roots are discovered
$expectedCorpusCount = "1788"

# Counters
$passed = 0
$failed = 0
$failedTests = @()

# Invariant tracking
$corpusLoweringOk = $false
$corpusLoweringCount = "?"
$linkerCorpusOk = $false
$linkerCorpusCount = "?"
$ownershipOk = $false
$decoderCfgOk = $false

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
        if ($details) {
            Write-Host "    $details" -ForegroundColor Gray
        }
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
if (-not (Test-Path -LiteralPath $RomPath)) {
    Write-Host "ERROR: ROM not found at: $RomPath" -ForegroundColor Red
    exit 1
}

# Verify build exists
if (-not (Test-Path $testReleaseDir)) {
    Write-Host "ERROR: Test build not found at: $testReleaseDir" -ForegroundColor Red
    Write-Host "Run the following to build:" -ForegroundColor Yellow
    Write-Host '  & "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release' -ForegroundColor Yellow
    exit 1
}

Write-Host "`nEngineemon Canonical Verification" -ForegroundColor White
Write-Host "ROM: $RomPath" -ForegroundColor Gray
Write-Host "Build: $buildDir" -ForegroundColor Gray

# =============================================================================
# Suite 1: Runtime Tests
# =============================================================================
Write-TestHeader "Runtime Tests"

$runtimeExe = "$testReleaseDir\runtime_test.exe"
if (Test-Path $runtimeExe) {
    # Run and capture output - stderr is visible but doesn't affect pass/fail
    # Pass/fail is determined ONLY by exit code
    $output = & $runtimeExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            # stderr - write to host but include in output for parsing
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    # Parse output for pass/fail count (informational only)
    $passedMatch = $output | Select-String -Pattern "^Passed:\s*(\d+)"
    $failedMatch = $output | Select-String -Pattern "^Failed:\s*(\d+)"
    
    if ($passedMatch -and $failedMatch) {
        $passCount = $passedMatch.Matches[0].Groups[1].Value
        $failCount = $failedMatch.Matches[0].Groups[1].Value
        # Success = exit code 0 (regardless of stderr output)
        Write-TestResult "runtime_test" ($exitCode -eq 0) "Passed: $passCount, Failed: $failCount"
    } else {
        Write-TestResult "runtime_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "runtime_test" $false "Executable not found"
}

# =============================================================================
# Suite 2: Golden Tests
# =============================================================================
Write-TestHeader "Golden Tests"

$goldenExe = "$testReleaseDir\golden_test.exe"
if (Test-Path $goldenExe) {
    # Run and capture output - pass/fail determined by exit code only
    $output = & $goldenExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    $passedMatch = $output | Select-String -Pattern "^Passed:\s*(\d+)"
    $failedMatch = $output | Select-String -Pattern "^Failed:\s*(\d+)"
    
    if ($passedMatch -and $failedMatch) {
        $passCount = $passedMatch.Matches[0].Groups[1].Value
        $failCount = $failedMatch.Matches[0].Groups[1].Value
        Write-TestResult "golden_test" ($exitCode -eq 0) "Passed: $passCount, Failed: $failCount"
    } else {
        Write-TestResult "golden_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "golden_test" $false "Executable not found"
}

# =============================================================================
# Suite 3: Legality Gate Tests (adversarial)
# =============================================================================
Write-TestHeader "Legality Gate Tests"

$legalityExe = "$testReleaseDir\legality_gate_test.exe"
if (Test-Path $legalityExe) {
    # Run and capture output - pass/fail determined by exit code only
    $output = & $legalityExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    $passedMatch = $output | Select-String -Pattern "^Passed:\s*(\d+)"
    $failedMatch = $output | Select-String -Pattern "^Failed:\s*(\d+)"
    
    if ($passedMatch -and $failedMatch) {
        $passCount = $passedMatch.Matches[0].Groups[1].Value
        $failCount = $failedMatch.Matches[0].Groups[1].Value
        Write-TestResult "legality_gate_test" ($exitCode -eq 0) "Passed: $passCount, Failed: $failCount"
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

$corpusExe = "$testReleaseDir\corpus_test.exe"
if (Test-Path $corpusExe) {
    # Run and capture output - pass/fail determined by exit code only
    $output = & $corpusExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    # Exit code 0 = all stages pass
    $decoderCfgOk = ($exitCode -eq 0)
    
    Write-TestResult "corpus_test" $decoderCfgOk "decoder/CFG integrity"
} else {
    Write-TestResult "corpus_test" $false "Executable not found"
}

# =============================================================================
# Suite 5: Corpus Lowering Audit (corpus=1788/1788 invariant)
# Stage 4: Semantic lowering verification
# =============================================================================
Write-TestHeader "Corpus Lowering Audit"

$loweringExe = "$toolsReleaseDir\corpus_lowering_audit.exe"
if (Test-Path $loweringExe) {
    # Run and capture output - pass/fail determined by exit code only
    $output = & $loweringExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    # Check for SUCCESS count
    $successMatch = $output | Select-String -Pattern "Successes:\s*(\d+)"
    $totalMatch = $output | Select-String -Pattern "Total:\s*(\d+)"
    
    if ($successMatch -and $totalMatch) {
        $corpusLoweringCount = $successMatch.Matches[0].Groups[1].Value
        $totalCount = $totalMatch.Matches[0].Groups[1].Value
        $corpusLoweringOk = ($corpusLoweringCount -eq $expectedCorpusCount) -and ($corpusLoweringCount -eq $totalCount)
        Write-TestResult "corpus_lowering_audit" ($exitCode -eq 0 -and $corpusLoweringOk) "lowering=$corpusLoweringCount/$expectedCorpusCount"
    } else {
        # Alternative: check for "ALL X BODIES COMPILE" pattern
        $allMatch = $output | Select-String -Pattern "ALL\s+(\d+)\s+BODIES\s+COMPILE"
        if ($allMatch) {
            $corpusLoweringCount = $allMatch.Matches[0].Groups[1].Value
            $corpusLoweringOk = ($corpusLoweringCount -eq $expectedCorpusCount)
            Write-TestResult "corpus_lowering_audit" ($exitCode -eq 0 -and $corpusLoweringOk) "lowering=$corpusLoweringCount/$expectedCorpusCount"
        } else {
            Write-TestResult "corpus_lowering_audit" ($exitCode -eq 0) "Exit code: $exitCode"
        }
    }
} else {
    Write-TestResult "corpus_lowering_audit" $false "Executable not found at $loweringExe"
}

# =============================================================================
# Suite 6: Linker Test (corpus=1788, InvalidOwnership=0)
# Stage 6: Corpus-wide typed-reference validation
# =============================================================================
Write-TestHeader "Linker Test"

$linkerExe = "$testReleaseDir\linker_test.exe"
if (Test-Path $linkerExe) {
    # Run and capture output - pass/fail determined by exit code only
    $output = & $linkerExe $RomPath 2>&1 | ForEach-Object { 
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE
    
    # Check corpus count
    $corpusMatch = $output | Select-String -Pattern "Total unique bodies:\s*(\d+)"
    if ($corpusMatch) {
        $linkerCorpusCount = $corpusMatch.Matches[0].Groups[1].Value
        $linkerCorpusOk = ($linkerCorpusCount -eq $expectedCorpusCount)
    }
    
    # Check InvalidOwnership count
    $ownerMatch = $output | Select-String -Pattern "InvalidOwnership:\s*(\d+)"
    if ($ownerMatch) {
        $ownerCount = $ownerMatch.Matches[0].Groups[1].Value
        $ownershipOk = ($ownerCount -eq "0")
    } else {
        # If not explicitly mentioned, check for absence of ownership errors
        $ownershipOk = -not ($output -match "InvalidOwnership:\s*[1-9]|ownership.?error|invalid.?owner")
    }
    
    $linkerPass = ($exitCode -eq 0) -and $linkerCorpusOk -and $ownershipOk
    $ownerDisplay = if ($ownershipOk) { "0" } else { "ERROR" }
    Write-TestResult "linker_test" $linkerPass "corpus=$linkerCorpusCount/$expectedCorpusCount, InvalidOwnership=$ownerDisplay"
} else {
    Write-TestResult "linker_test" $false "Executable not found"
}

# =============================================================================
# Suite 7: Compiler Integrity Tests (fail-open adversarial)
# Finding 1: asset extraction fail-closed
# Finding 2: map discovery fail-closed
# =============================================================================
Write-TestHeader "Compiler Integrity Tests"

$integrityExe = "$testReleaseDir\compiler_integrity_test.exe"
if (Test-Path $integrityExe) {
    $output = & $integrityExe $RomPath 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE

    $passedMatch = $output | Select-String -Pattern "^Passed:\s*(\d+)"
    $failedMatch = $output | Select-String -Pattern "^Failed:\s*(\d+)"

    if ($passedMatch -and $failedMatch) {
        $passCount = $passedMatch.Matches[0].Groups[1].Value
        $failCount = $failedMatch.Matches[0].Groups[1].Value
        Write-TestResult "compiler_integrity_test" ($exitCode -eq 0) "Passed: $passCount, Failed: $failCount"
    } else {
        Write-TestResult "compiler_integrity_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "compiler_integrity_test" $false "Executable not found"
}

# =============================================================================
# Suite 8: Crystal Frontend Oracle (Phase 1)
# Independent source-fidelity tests — expected values from pokecrystal semantics,
# never from Enginemon encoder/decoder output.
# =============================================================================
Write-TestHeader "Crystal Frontend Oracle (Phase 1)"

$oracleExe = "$testReleaseDir\oracle_test.exe"
if (Test-Path $oracleExe) {
    $output = & $oracleExe $RomPath 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            $_
        }
    }
    $exitCode = $LASTEXITCODE

    $passedMatch = $output | Select-String -Pattern "^Passed:\s*(\d+)"
    $failedMatch = $output | Select-String -Pattern "^Failed:\s*(\d+)"

    if ($passedMatch -and $failedMatch) {
        $passCount = $passedMatch.Matches[0].Groups[1].Value
        $failCount = $failedMatch.Matches[0].Groups[1].Value
        Write-TestResult "oracle_test" ($exitCode -eq 0) "Passed: $passCount, Failed: $failCount"
    } else {
        Write-TestResult "oracle_test" ($exitCode -eq 0) "Exit code: $exitCode"
    }
} else {
    Write-TestResult "oracle_test" $false "Executable not found"
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
Write-Host "    corpus lowering   = $corpusLoweringCount/$expectedCorpusCount" -ForegroundColor $(if ($corpusLoweringOk) { "Green" } else { "Red" })
Write-Host "    linker corpus     = $linkerCorpusCount/$expectedCorpusCount" -ForegroundColor $(if ($linkerCorpusOk) { "Green" } else { "Red" })
$ownerDisplay = if ($ownershipOk) { "0" } else { "ERROR" }
Write-Host "    InvalidOwnership  = $ownerDisplay" -ForegroundColor $(if ($ownershipOk) { "Green" } else { "Red" })
$decoderDisplay = if ($decoderCfgOk) { "PASS" } else { "FAIL" }
Write-Host "    decoder/CFG       = $decoderDisplay" -ForegroundColor $(if ($decoderCfgOk) { "Green" } else { "Red" })

# Final result
Write-Host "`n" -NoNewline
$allInvariantsOk = $corpusLoweringOk -and $linkerCorpusOk -and $ownershipOk -and $decoderCfgOk
if ($failed -eq 0 -and $allInvariantsOk) {
    Write-Host "  OVERALL: PASS" -ForegroundColor Green -BackgroundColor DarkGreen
    Write-Host ""
    exit 0
} else {
    Write-Host "  OVERALL: FAIL" -ForegroundColor Red -BackgroundColor DarkRed
    Write-Host ""
    exit 1
}
