# run_all_tests.ps1
# Canonical Enginemon verification script
# Runs every correctness-relevant test suite and the production runtime compile gates.
#
# Required canonical gates:
#   0.  build                    (single cmake --build invocation for all targets)
#   1.  enginemon_bootstrap      build gate
#       enginemon_tiles          build gate
#   2.  runtime_test_engine      (engine-only, 194 tests)
#   3.  runtime_test_compiler    (compiler/IR, 392 tests)
#   4.  battle_test              (engine-only, no ROM)
#   5.  emitter_test             (no ROM)
#   6.  sprite_money_test        (standalone, no ROM)
#   7.  crystal_save_test        (standalone, no ROM)
#   8.  golden_test
#   9.  legality_gate_test       (adversarial, 16 tests)
#  10.  corpus_test              (decoder/CFG integrity)
#  11.  corpus_lowering_audit    (1788/1788 invariant)
#  12.  linker_test              (corpus=1788, InvalidOwnership=0)
#  13.  compiler_integrity_test
#  14.  oracle_test              (Phases 1-5.5 full-pipe -- SKIP = FAIL)
#  15.  profile_verify           (profile offset correctness)
#  16.  trainer_verify           (trainer group extraction correctness)
#  17.  emon_smoke               (fresh ROM->compile->smoke)
#
# NOTE: runtime_test (monolithic) is intentionally excluded from the canonical
# build.  Coverage is complete: runtime_test_engine (194) + runtime_test_compiler
# (392) = 586 tests covering all 11 TUs.  The monolithic target remains in
# CMakeLists.txt for local use but must not be rebuilt during the canonical run.
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
#   .\run_all_tests.ps1 -RomPath "..." -NoBuild   # skip build, use existing binaries

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
# Single cmake --build invocation builds all canonical targets in one MSBuild
# session.  Shared dependencies (enginemon_engine, enginemon_crystal) are built
# once and reused; no redundant dependency-graph evaluation.
# runtime_test (monolith) is deliberately excluded â€” coverage is complete via
# runtime_test_engine + runtime_test_compiler.
# ---------------------------------------------------------------------------
Write-Header "Build"

# Canonical binaries that must exist for the gates below.
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

    # All canonical targets in one MSBuild invocation.
    # MSBuild schedules shared dependencies (enginemon_engine, enginemon_crystal)
    # once and parallelises independent leaf targets.
    Write-Host "  Building all canonical targets..." -ForegroundColor Gray
    & $cmake --build $buildDir --config Release `
        --target runtime_test_engine `
        --target runtime_test_compiler `
        --target battle_test `
        --target emitter_test `
        --target sprite_money_test `
        --target crystal_save_test `
        --target golden_test `
        --target legality_gate_test `
        --target corpus_test `
        --target linker_test `
        --target compiler_integrity_test `
        --target oracle_test `
        --target corpus_lowering_audit `
        --target profile_verify `
        --target trainer_verify `
        --target enginemon_bootstrap `
        --target enginemon_tiles `
        --target emon_smoke `
        --target emon_compile `
        2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }

    if ($LASTEXITCODE -ne 0) {
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
# GATE 2 -- Runtime Tests (engine)
# Engine-only TUs; no Crystal compiler IR variants instantiated.
# 194 tests covering: core, maps, blast_radius, integration, warp_semantics, rtc
# ---------------------------------------------------------------------------
Write-Header "Runtime Tests (engine)"

$rtEngineExe = "$testDir\runtime_test_engine.exe"
if (Test-Path $rtEngineExe) {
    $r  = Invoke-Exe $rtEngineExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "runtime_test_engine" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "runtime_test_engine" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "runtime_test_engine" $false "Executable not found: $rtEngineExe"
}

# ---------------------------------------------------------------------------
# GATE 3 -- Runtime Tests (compiler)
# Crystal compiler/IR TUs (SemanticOp + CrystalCommandData variants).
# 392 tests covering: scripts, batches, vm_state, text_rng, pcg_vm
# ---------------------------------------------------------------------------
Write-Header "Runtime Tests (compiler)"

$rtCompilerExe = "$testDir\runtime_test_compiler.exe"
if (Test-Path $rtCompilerExe) {
    $r  = Invoke-Exe $rtCompilerExe @($RomPath)
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "runtime_test_compiler" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "runtime_test_compiler" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "runtime_test_compiler" $false "Executable not found: $rtCompilerExe"
}

# ---------------------------------------------------------------------------
# GATE 4 -- Battle Tests (engine-only, no ROM needed)
# ---------------------------------------------------------------------------
Write-Header "Battle Tests"

$battleExe = "$testDir\battle_test.exe"
if (Test-Path $battleExe) {
    $r  = Invoke-Exe $battleExe @()
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "battle_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "battle_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "battle_test" $false "Executable not found: $battleExe"
}

# ---------------------------------------------------------------------------
# GATE 5 -- Emitter Tests  (no ROM argument)
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
# GATE 6 -- Sprite/Money Tests  (no ROM argument)
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
# GATE 7 -- Crystal Save Tests  (standalone, no ROM)
# Crystal .sav codec round-trip, checksum, shadow preservation.
# ---------------------------------------------------------------------------
Write-Header "Crystal Save Tests"

$crystalSaveExe = "$testDir\crystal_save_test.exe"
if (Test-Path $crystalSaveExe) {
    $r  = Invoke-Exe $crystalSaveExe @()
    $pf = Get-PassFail $r.Lines
    if ($pf.PassCount -ne $null -and $pf.FailCount -ne $null) {
        Write-Result "crystal_save_test" ($r.ExitCode -eq 0) "Passed: $($pf.PassCount), Failed: $($pf.FailCount)"
    } else {
        Write-Result "crystal_save_test" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
    }
} else {
    Write-Result "crystal_save_test" $false "Executable not found: $crystalSaveExe"
}

# ---------------------------------------------------------------------------
# GATE 8 -- Golden Tests
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
# GATE 9 -- Legality Gate Tests (adversarial)
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
# GATE 10 -- Corpus Test (Decoder/CFG Integrity)
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
# GATE 11 -- Corpus Lowering Audit  (1788/1788 invariant)
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
# GATE 12 -- Linker Test  (corpus=1788, InvalidOwnership=0)
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
# GATE 13 -- Compiler Integrity Tests
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
# GATE 14 -- Crystal Frontend Oracle (Phases 1-5.5)
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
# GATE 15 -- Profile Verify
# Validates ExtractionProfile base-data and move offsets against known-good
# values from the Crystal ROM.  Returns 1 on any stat mismatch.
# ---------------------------------------------------------------------------
Write-Header "Profile Verify"

$profileExe = "$toolsDir\profile_verify.exe"
if (Test-Path $profileExe) {
    $r = Invoke-Exe $profileExe @($RomPath)
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
# GATE 16 -- Trainer Verify
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
# GATE 17 -- Production Runtime Smoke
# Compiles a FRESH temporary .emon from the same ROM used by all other gates,
# then runs emon_smoke against that package.  This proves the smoke exercises
# the current compiler output, not an arbitrary pre-existing crystal.emon.
#
# Flow:  RomPath -> emon_compile -> $env:TEMP\smoke_verify.emon -> emon_smoke
#        Temporary package is removed after the gate regardless of outcome.
# ---------------------------------------------------------------------------
Write-Header "Production Runtime Smoke"

$smokeExe    = "$toolsDir\emon_smoke.exe"
$emonCompile = "$toolsDir\emon_compile.exe"
$smokePkg    = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "smoke_verify_$([System.Diagnostics.Process]::GetCurrentProcess().Id).emon")

if (-not (Test-Path $smokeExe)) {
    Write-Result "emon_smoke" $false "Executable not found: $smokeExe"
} elseif (-not (Test-Path $emonCompile)) {
    Write-Result "emon_smoke" $false "Executable not found: $emonCompile (cannot compile fresh package)"
} else {
    # Step 1: compile a fresh temporary package from the canonical ROM
    $compileR = Invoke-Exe $emonCompile @($RomPath, $smokePkg, "--no-cache")
    if ($compileR.ExitCode -ne 0 -or -not (Test-Path $smokePkg)) {
        Write-Result "emon_smoke" $false "emon_compile failed (exit $($compileR.ExitCode)) -- cannot verify smoke provenance"
    } else {
        # Step 2: run smoke against the freshly compiled package
        $r = Invoke-Exe $smokeExe @($smokePkg)
        $passLine = $r.Lines | Select-String -Pattern "\[smoke\] PASS" | Select-Object -First 1
        $failLine = $r.Lines | Select-String -Pattern "\[smoke\] FAIL" | Select-Object -First 1
        if ($passLine) {
            Write-Result "emon_smoke" $true "Fresh ROM->compile->smoke passed (package was $([math]::Round((Get-Item $smokePkg).Length/1KB)) KB)"
        } elseif ($failLine) {
            Write-Result "emon_smoke" $false $failLine.Line.Trim()
        } else {
            Write-Result "emon_smoke" ($r.ExitCode -eq 0) "Exit code: $($r.ExitCode)"
        }
    }
    # Step 3: always clean up the temporary package
    Remove-Item -LiteralPath $smokePkg -Force -ErrorAction SilentlyContinue
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
