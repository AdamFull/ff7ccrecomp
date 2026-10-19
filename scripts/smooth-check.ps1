# Smooth motion check: replays recordings unthrottled with --smooth-check (every picture in between drawn) and lists
# the draws it flags: matched 3D draws drawn apart from a part of a model next to them that moved as they did (a face's
# lips below its chin). Fails when any are flagged. Logs stay in -Out (they name vblanks to look at with
# --window-shot).
#
#   scripts\smooth-check.ps1
#   scripts\smooth-check.ps1 -Runs load,menus
param(
    [string[]]$Runs = @('load', 'long', 'menus'),
    [string]$Out = 'build\smooth-check',
    [string]$Dir = 'data\recordings',
    [string]$Exe = 'build\windows-x64-release\ff7cc.exe',
    [string]$GameDir = 'data\original',
    [string]$CookedDir = 'data\cooked'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Test-Path $Exe)) { throw "$Exe not found; build first (scripts\build.ps1 -Preset windows-x64-release)" }
    $exe = (Resolve-Path $Exe).Path
    $game = (Resolve-Path $GameDir).Path
    $cooked = (Resolve-Path $CookedDir).Path
    $recordings = (Resolve-Path $Dir).Path
    New-Item -ItemType Directory -Force $Out | Out-Null
    $out = (Resolve-Path $Out).Path

    $ini = Join-Path $out 'smooth-check.ini'
    Set-Content -Encoding ascii $ini @('renderer=vulkan', 'fullscreen=0', 'volume=0', 'frame_limit=1', 'vsync=1',
                                       'refresh_60hz=1', 'show_fps=0', 'frame_skip=1', 'hd_textures=1',
                                       'fill_screen=0', 'smooth_motion=1', 'msaa=1', 'scale=2')
    # load.txt ends in the opening cutscene's dialogue: Cross every 2 s carries it on (to the ceiling lights, ~4640).
    $dialogue = @(for ($v = 4150; $v -lt 7000; $v += 120) { '--press'; "${v}:cross" })
    # name: recording, last vblank, extra arguments
    $all = @{
        load = @('load.txt', 7000, $dialogue)
        battle = @('battle.txt', 18800, @())
        long = @('long.txt', 21900, @())  # (starts as battle.txt)
        menus = @('menus.txt', 33000, @())
    }
    Set-Location (Join-Path $root 'run')  # (test games run from run\: its pipeline cache and settings)
    $flagged = 0
    foreach ($name in $Runs) {
        if (-not $all.ContainsKey($name)) { throw "unknown run $name (load, battle, long, menus)" }
        $rec, $last, $extra = $all[$name]
        $log = Join-Path $out "$name.log"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $ErrorActionPreference = 'Continue'
        & $exe $game --cooked $cooked --no-audio --unthrottled --smooth-check --settings $ini `
            --replay (Join-Path $recordings $rec) --vblanks $last @extra 2>&1 | Out-File -Width 400 $log
        $ErrorActionPreference = 'Stop'
        $lines = @(Select-String -Path $log -Pattern 'smooth check, ' | ForEach-Object { $_.Line })
        $flagged += $lines.Count
        Write-Host ("{0,-6} {1,4} flagged  {2:0} s" -f $name, $lines.Count, $sw.Elapsed.TotalSeconds)
        $lines | Select-Object -First 10 | ForEach-Object { Write-Host "  $($_ -replace '^.*smooth check, ', '')" }
    }
    if ($flagged) { throw "$flagged draws flagged (logs in $Out)" }
    Write-Host 'nothing flagged'
} finally {
    Pop-Location
}
