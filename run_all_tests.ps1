# run_all_tests.ps1
# Canonical Enginemon verification script
# Runs every correctness-relevant test suite and the production runtime compile gates.
#
# Required canonical gates:
#   1.  runtime_test
#   2.  emitter_test
#   3.  sprite_money_test        (standalone, no ROM)
#   4.  golden_test
#   5.  legality_gate_test
#   6.  corpus_test
#   7.  corpus_lowering_audit
#   8.  linker_test
#   9.  compiler_integrity_test
#  10.  oracle_test              (Phases 1-5.5 full-pipe -- SKIP = FAIL)
#  11.  profile_verify           (profile offset correctness)
#  12.  trainer_verify           (trainer group extraction correctness)
#  13.  enginemon_bootstrap      build gate
#  14.  enginemon_tiles          build gate
#
# Required invariants:
#   - corpus lowering = 1788/1788
#   - linker corpus   = 1788/1788
#   - InvalidOwnership = 0
#   - decoder/CFG integrity = PASS
#   - All test suites pass
#   - No required suite is skipped, omitted, or fails to build
#
# Exit codes:
#   0 = ALL PASS
#   1 = FAILURE (details reported)
#
# Usage:
#   .\run_all_tests.ps1 -RomPath "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"

param(
    [Parameter(Mandatory=$true)]
    [string]$RomPath,

    [switch]$NoBuild   # Skip the build step (assumes binaries are already up-to-date)
)

$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
$cmake           = "C:\Program Files\CMake\bin\cmake.exe"
$vcpkgToolchain  = "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
$buildDir        = "build"
$testDir         = "$buildDir\tests\Release"
$toolsDir        = "$buildDir\tools\Release"
$runtimeDir      = "$buildDir\runtime\Release"

$expectedCorpusCount = "1788"

$passed      = 0
$failed      = 0
$failedTests = @()

$corpusLoweringOk    = $false
$corpusLoweringCount = "?"
$linkerCorpusOk      = $false
$linkerCorpusCount   = "?"
$ownershipOk         = $false
$decoderCfgOk        = $false

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Header($name) {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host "  $name" -ForegroundColor Cyan
    Write-Host ("=" * 60) -ForegroundColor Cyan
}

function Write-Result($name, $success, $details = "") {
    if ($success) {
        Write-Host "  [$name] " -NoNewline
        Write-Host "PASS" -ForegroundColor Green
        if ($details) { Write-Host "    $details" -ForegroundColor Gray }
        $script:passed++
    } else {
        Write-Host "  [$name] " -NoNewline
        Write-Host "FAIL" -ForegroundColor Red
        if ($details) { Write-Host "    $details" -ForegroundColor Yellow }
        $script:failed++
        $script:failedTests += $name
    }
}

# Run an executable; capture merged stdout+stderr; return hashtable with
# ExitCode and Lines[].
function Invoke-Exe {
    param([string]$Exe, [string[]]$ExeArgs)
    $lines = @()
    & $Exe @ExeArgs 2>&1 | ForEach-Object {
        $text = if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.Exception.Message -ForegroundColor DarkGray
            $_.Exception.Message
        } else {
            "$_"
        }
        $lines += $text
    }
    return @{ ExitCode = $LASTEXITCODE; Lines = $lines }
}

# Extract Passed/Failed counts from output lines.
function Get-PassFail($lines) {
    $p = ($lines | Select-String -Pattern "^Passed:\s*(\d+)") | Select-Object -First 1
    $f = ($lines | Select-String -Pattern "^Failed:\s*(\d+)") | Select-Object -First 1
    return @{
        PassCount = if ($p) { $p.Matches[0].Groups[1].Value } else { $null }
        FailCount = if ($f) { $f.Matches[0].Groups[1].Value } else { $null }
    }
}

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $RomPath)) {
    Write-Host "ERROR: ROM not found: $RomPath" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Enginemon Canonical Verification" -ForegroundColor White
Write-Host "ROM:   $RomPath"          -ForegroundColor Gray
Write-Host "Build: $buildDir"         -ForegroundColor Gray
Write-Host "Date:  $(Get-Date -Format 'yyyy-MM-dd HH:mm')" -ForegroundColor Gray

# ---------------------------------------------------------------------------
# GATE 0 -- Build
# ---------------------------------------------------------------------------
Write-Header "Build"

$requiredExes = @(
    "$testDir\runtime_test.exe",
    "$testDir\emitter_test.exe",
    "$testDir\sprite_money_test.exe",
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
    "$runtimeDir\enginemon_tiles.exe"
)

if ($NoBuild) {
    Write-Host "  [build] SKIPPED (--NoBuild)" -ForegroundColor Yellow
    $missingExes = $requiredExes | Where-Object { -not (Test-Path $_) }
    if ($missingExes.Count -gt 0) {
        Write-Host "  ERROR: Required binaries missing (run without --NoBuild):" -ForegroundColor Red
        $missingExes | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        exit 1
    }
    Write-Host "  [build] All required binaries present" -ForegroundColor Gray
} else {
    if (-not (Test-Path $cmake)) {
        Write-Host "  ERROR: CMake not found at $cmake" -ForegroundColor Red
        exit 1
    }

    if (-not (Test-Path $buildDir)) {
        Write-Host "  Configuring CMake build..." -ForegroundColor Gray
        & $cmake `
            -B $buildDir `
            -DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain `
            -DENGINEMON_ENABLE_ENGINE=ON `
            -DENGINEMON_ENABLE_VULKAN=ON `
            2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [build] FAIL: CMake configure failed" -ForegroundColor Red
            exit 1
        }
    }

    $buildTargets = @(
        "runtime_test", "emitter_test", "sprite_money_test",
        "golden_test", "legality_gate_test", "corpus_test",
        "linker_test", "compiler_integrity_test", "oracle_test",
        "corpus_lowering_audit", "profile_verify", "trainer_verify",
        "enginemon_bootstrap", "enginemon_tiles"
    )

    $buildFailed = $false
    foreach ($tgt in $buildTargets) {
        Write-Host "  Building $tgt..." -ForegroundColor Gray
        & $cmake --build $buildDir --target $tgt --config Release 2>&1 |
            ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [build] FAIL: target '$tgt' did not build cleanly" -ForegroundColor Red
            $buildFailed = $true
        }
    }

    if ($buildFailed) {
        Write-Host ""
        Write-Host "  FATAL: build failure -- aborting verification." -ForegroundColor Red
        exit 1
    }
    Write-Host "  [build] All required targets built" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# GATE 1 -- Production Runtime Compile Gates
# ---------------------------------------------------------------------------
Write-Header "Production Runtime Compile Gates"

$bootstrapExe = "$runtimeDir\enginemon_bootstrap.exe"
if (Test-Path $bootstrapExe) {
    $sz = [math]::Round((Get-Item $bootstrapExe).Length / 1KB)
    Write-Result "enginemon_bootstrap_compile" $true "enginemon_bootstrap.exe (${sz} KB)"
} else {
    Write-Result "enginemon_bootstrap_compile" $false "enginemon_bootstrap.exe not found at $bootstrapExe"
}

$tilesExe = "$runtimeDir\enginemon_tiles.exe"
if (Test-Path $tilesExe) {
    $sz = [math]::Round((Get-Item $tilesExe).Length / 1KB)
    Write-Result "enginemon_tiles_compile" $true "enginemon_tiles.exe (${sz} KB)"
} else {
    Write-Result "enginemon_tiles_compile" $false "enginemon_tiles.exe not found at $tilesExe"
}

# ---------------------------------------------------------------------------
# GATE 2 -- Runtime Tests
# ---------------------------------------------------------------------------
Write-Header "Runtime Tests"

$runtimeExe = "$testDir\runtime_test.exe"
if (Test-Path $runtimeExe) {
    $r  = Invoke-Exe $runtimeExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "runtime_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "runtime_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "runtime_test" $false "Executable not found: $runtimeExe"
}

# ---------------------------------------------------------------------------
# GATE 3 -- Emitter Tests  (no ROM argument)
# ---------------------------------------------------------------------------
Write-Header "Emitter Tests"

$emitterExe = "$testDir\emitter_test.exe"
if (Test-Path $emitterExe) {
    $r  = Invoke-Exe $emitterExe @()
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "emitter_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "emitter_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "emitter_test" $false "Executable not found: $emitterExe"
}

# ---------------------------------------------------------------------------
# GATE 4 -- Sprite/Money Tests  (no ROM argument)
# Covers GameState variable-sprite identity, money account isolation,
# Day Care species save/load, and PackageWriter->PackageReader species-icon roundtrip.
# ---------------------------------------------------------------------------
Write-Header "Sprite/Money Tests"

$spriteMoneyExe = "$testDir\sprite_money_test.exe"
if (Test-Path $spriteMoneyExe) {
    $r  = Invoke-Exe $spriteMoneyExe @()
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "sprite_money_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "sprite_money_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "sprite_money_test" $false "Executable not found: $spriteMoneyExe"
}

# ---------------------------------------------------------------------------
# GATE 5 -- Golden Tests
# ---------------------------------------------------------------------------
Write-Header "Golden Tests"

$goldenExe = "$testDir\golden_test.exe"
if (Test-Path $goldenExe) {
    $r  = Invoke-Exe $goldenExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "golden_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "golden_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "golden_test" $false "Executable not found: $goldenExe"
}

# ---------------------------------------------------------------------------
# GATE 6 -- Legality Gate Tests (adversarial)
# ---------------------------------------------------------------------------
Write-Header "Legality Gate Tests"

$legalityExe = "$testDir\legality_gate_test.exe"
if (Test-Path $legalityExe) {
    $r  = Invoke-Exe $legalityExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "legality_gate_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "legality_gate_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "legality_gate_test" $false "Executable not found: $legalityExe"
}

# ---------------------------------------------------------------------------
# GATE 7 -- Corpus Test (Decoder/CFG Integrity)
# ---------------------------------------------------------------------------
Write-Header "Corpus Test (Decoder/CFG Integrity)"

$corpusExe = "$testDir\corpus_test.exe"
if (Test-Path $corpusExe) {
    $r = Invoke-Exe $corpusExe @($RomPath)
    $decoderCfgOk = ($r.ExitCode -eq 0)
    Write-Result "corpus_test" $decoderCfgOk "decoder/CFG integrity -- Exit code: $($r.ExitCode)"
} else {
    Write-Result "corpus_test" $false "Executable not found: $corpusExe"
}

# ---------------------------------------------------------------------------
# GATE 8 -- Corpus Lowering Audit  (1788/1788 invariant)
# ---------------------------------------------------------------------------
Write-Header "Corpus Lowering Audit"

$loweringExe = "$toolsDir\corpus_lowering_audit.exe"
if (Test-Path $loweringExe) {
    $r = Invoke-Exe $loweringExe @($RomPath)

    $successM = $r.Lines | Select-String -Pattern "Successes:\s*(\d+)"   | Select-Object -First 1
    $totalM   = $r.Lines | Select-String -Pattern "Total:\s*(\d+)"       | Select-Object -First 1
    $allM     = $r.Lines | Select-String -Pattern "ALL\s+(\d+)\s+BODIES" | Select-Object -First 1

    if ($successM -and $totalM) {
        $corpusLoweringCount = $successM.Matches[0].Groups[1].Value
        $totalCount          = $totalM.Matches[0].Groups[1].Value
        $corpusLoweringOk    = ($corpusLoweringCount -eq $expectedCorpusCount) -and ($corpusLoweringCount -eq $totalCount)
    } elseif ($allM) {
        $corpusLoweringCount = $allM.Matches[0].Groups[1].Value
        $corpusLoweringOk    = ($corpusLoweringCount -eq $expectedCorpusCount)
    }

    Write-Result "corpus_lowering_audit" ($r.ExitCode -eq 0 -and $corpusLoweringOk) `
        "lowering=$corpusLoweringCount/$expectedCorpusCount"
} else {
    Write-Result "corpus_lowering_audit" $false "Executable not found: $loweringExe"
}

# ---------------------------------------------------------------------------
# GATE 9 -- Linker Test  (corpus=1788, InvalidOwnership=0)
# ---------------------------------------------------------------------------
Write-Header "Linker Test"

$linkerExe = "$testDir\linker_test.exe"
if (Test-Path $linkerExe) {
    $r = Invoke-Exe $linkerExe @($RomPath)

    $corpusM = $r.Lines | Select-String -Pattern "Total unique bodies:\s*(\d+)" | Select-Object -First 1
    $ownerM  = $r.Lines | Select-String -Pattern "InvalidOwnership:\s*(\d+)"    | Select-Object -First 1

    if ($corpusM) {
        $linkerCorpusCount = $corpusM.Matches[0].Groups[1].Value
        $linkerCorpusOk    = ($linkerCorpusCount -eq $expectedCorpusCount)
    }
    if ($ownerM) {
        $ownershipOk = ($ownerM.Matches[0].Groups[1].Value -eq "0")
    } else {
        $ownershipOk = -not ($r.Lines -match "InvalidOwnership:\s*[1-9]|ownership.?error|invalid.?owner")
    }

    $ownerDisp = if ($ownershipOk) { "0" } else { "ERROR" }
    Write-Result "linker_test" ($r.ExitCode -eq 0 -and $linkerCorpusOk -and $ownershipOk) `
        "corpus=$linkerCorpusCount/$expectedCorpusCount, InvalidOwnership=$ownerDisp"
} else {
    Write-Result "linker_test" $false "Executable not found: $linkerExe"
}

# ---------------------------------------------------------------------------
# GATE 10 -- Compiler Integrity Tests
# ---------------------------------------------------------------------------
Write-Header "Compiler Integrity Tests"

$integrityExe = "$testDir\compiler_integrity_test.exe"
if (Test-Path $integrityExe) {
    $r  = Invoke-Exe $integrityExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "compiler_integrity_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "compiler_integrity_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "compiler_integrity_test" $false "Executable not found: $integrityExe"
}

# ---------------------------------------------------------------------------
# GATE 11 -- Crystal Frontend Oracle (Phases 1-5.5)
#
# SKIP policy (hard rule):
#   Any SKIP output emitted by the oracle binary is a canonical failure.
#   A skipped required correctness test is NOT an acceptable outcome.
#
#   Patterns matched:
#     "[Phase 5] SKIP: ..."    -- whole P5 suite skipped (package compile failed)
#     "[Phase 5.5] SKIP: ..."  -- whole P5.5 suite skipped
#     ": SKIP -..."            -- individual test self-skipped
# ---------------------------------------------------------------------------
Write-Header "Crystal Frontend Oracle (Phases 1-5.5)"

$oracleExe = "$testDir\oracle_test.exe"
if (Test-Path $oracleExe) {
    $r  = Invoke-Exe $oracleExe @($RomPath)
    $pf = Get-PassFail $r.Lines

    $canonicalSkips = $r.Lines | Where-Object {
        ($_ -match "\[Phase 5") -and ($_ -match "SKIP") -or
        ($_ -match ":\s+SKIP\s+-")
    }

    $hasSkips = ($canonicalSkips.Count -gt 0)
    $oracleOk = ($r.ExitCode -eq 0) -and (-not $hasSkips)

    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        $detail = "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        $detail = "Exit code: $($r.ExitCode)"
    }

    if ($hasSkips) {
        $detail += " | SKIPPED TESTS DETECTED (= FAIL)"
        $canonicalSkips | Select-Object -First 5 | ForEach-Object {
            Write-Host "    SKIP: $_" -ForegroundColor Yellow
        }
    }

    Write-Result "oracle_test" $oracleOk $detail
} else {
    Write-Result "oracle_test" $false "Executable not found: $oracleExe"
}

# ---------------------------------------------------------------------------
# GATE 12 -- Profile Verify
# Validates ExtractionProfile base-data and move offsets against known-good
# values from the Crystal ROM.  Returns 1 on any stat mismatch.
# ---------------------------------------------------------------------------
Write-Header "Profile Verify"

$profileExe = "$toolsDir\profile_verify.exe"
if (Test-Path $profileExe) {
    $r = Invoke-Exe $profileExe @($RomPath)
    # Tool reports "Profile verification: PASSED" or "FAILED"
    $passLine = $r.Lines | Select-String -Pattern "Profile verification:\s*PASSED" | Select-Object -First 1
    $failLine = $r.Lines | Select-String -Pattern "Profile verification:\s*FAILED" | Select-Object -First 1
    if ($passLine) {
        Write-Result "profile_verify" $true "Profile offsets match ROM data"
    } elseif ($failLine) {
        Write-Result "profile_verify" $false "Profile offset mismatch -- see output above"
    } else {
        Write-Result "profile_verify" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "profile_verify" $false "Executable not found: $profileExe"
}

# ---------------------------------------------------------------------------
# GATE 13 -- Trainer Verify
# Validates TrainerRegistry extraction counts against authoritative
# per-group counts from pokecrystal/data/trainers/parties.asm.
# Returns 1 on any group count mismatch.
# ---------------------------------------------------------------------------
Write-Header "Trainer Verify"

$trainerExe = "$toolsDir\trainer_verify.exe"
if (Test-Path $trainerExe) {
    $r = Invoke-Exe $trainerExe @($RomPath)
    $verifiedLine = $r.Lines | Select-String -Pattern "VERIFIED.*All counts match" | Select-Object -First 1
    $bugLine      = $r.Lines | Select-String -Pattern "BUG FOUND"                  | Select-Object -First 1
    if ($verifiedLine) {
        Write-Result "trainer_verify" $true "All trainer group counts match authoritative source"
    } elseif ($bugLine) {
        Write-Result "trainer_verify" $false "Trainer count mismatch -- see output above"
    } else {
        Write-Result "trainer_verify" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "trainer_verify" $false "Executable not found: $trainerExe"
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor White
Write-Host "  SUMMARY" -ForegroundColor White
Write-Host ("=" * 60) -ForegroundColor White
Write-Host ""
Write-Host "  Gates passed: $passed" -ForegroundColor Green
Write-Host "  Gates failed: $failed" `
    -ForegroundColor $(if ($failed -gt 0) { "Red" } else { "Green" })

if ($failed -gt 0) {
    Write-Host ""
    Write-Host "  Failed gates:" -ForegroundColor Red
    foreach ($t in $failedTests) {
        Write-Host "    - $t" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "  Key Invariants:" -ForegroundColor Gray
Write-Host "    corpus lowering   = $corpusLoweringCount/$expectedCorpusCount" `
    -ForegroundColor $(if ($corpusLoweringOk) { "Green" } else { "Red" })
Write-Host "    linker corpus     = $linkerCorpusCount/$expectedCorpusCount" `
    -ForegroundColor $(if ($linkerCorpusOk) { "Green" } else { "Red" })
$ownerDisp2 = if ($ownershipOk) { "0" } else { "ERROR" }
Write-Host "    InvalidOwnership  = $ownerDisp2" `
    -ForegroundColor $(if ($ownershipOk) { "Green" } else { "Red" })
$decoderDisp = if ($decoderCfgOk) { "PASS" } else { "FAIL" }
Write-Host "    decoder/CFG       = $decoderDisp" `
    -ForegroundColor $(if ($decoderCfgOk) { "Green" } else { "Red" })
Write-Host ""

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