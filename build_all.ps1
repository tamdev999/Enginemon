# build_all.ps1
# Compatibility shim. Canonical logic lives in tools/emon.py.
#
# Usage:
#   .\build_all.ps1                   # build all
#   .\build_all.ps1 -Preset engine    # subsystem
#   .\build_all.ps1 -Clean            # clean then build
#
# Preferred: python tools/emon.py build [--preset NAME] [--clean]

param(
    [string]$Preset = "all",
    [switch]$Clean,
    [switch]$Configure
)

$args_list = @("build", "--preset", $Preset)
if ($Clean)     { $args_list += "--clean" }
if ($Configure) { $args_list += "--configure" }

python "tools/emon.py" @args_list
exit $LASTEXITCODE
