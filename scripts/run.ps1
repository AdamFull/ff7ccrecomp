# Runs the game with the repository's layout: the disc from data\original, cooked media from data\cooked, and run\
# as the working directory (settings, saves, mods, the shader cache, dumps). Other arguments go to the game; paths
# in them are relative to run\.
#
#   scripts\run.ps1                                                    # play
#   scripts\run.ps1 --replay ..\data\recordings\long.txt --unthrottled  # replay a recording
#   scripts\run.ps1 -Preset windows-x64-debug --perf
param(
    [string]$Preset = 'windows-x64-release',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Preset\ff7cc.exe"
if (-not (Test-Path $exe)) { throw "$exe not found; build first (scripts\build.ps1 -Preset $Preset)" }
$run = Join-Path $root 'run'
New-Item -ItemType Directory -Force $run | Out-Null
Push-Location $run
try {
    & $exe (Join-Path $root 'data\original') --cooked (Join-Path $root 'data\cooked') @GameArgs
} finally {
    Pop-Location
}
