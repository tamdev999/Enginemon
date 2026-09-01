# run_all_tests.ps1
# Compatibility shim. Canonical logic lives in tools/emon.py.
#
# Usage:
#   .\run_all_tests.ps1 -RomPath "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
#   .\run_all_tests.ps1 -RomPath "..." -NoBuild
#   .\run_all_tests.ps1 -RomPath "..." -Filter engine
#
# Preferred: python tools/emon.py verify [--rom <path>] [--no-build]

param(
    [Parameter(Mandatory=$true)][string]$RomPath,
    [switch]$NoBuild,
    [string]$Filter = "all"
)

$args_list = @("verify", "--rom", $RomPath)
if ($NoBuild)           { $args_list += "--no-build" }
if ($Filter -ne "all")  { Write-Host "Note: use 'python tools/emon.py test --preset $Filter' for subsystem filtering" }

python "tools/emon.py" @args_list
exit $LASTEXITCODE
