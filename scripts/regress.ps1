# Regression check: replays every recording headless and compares the game's RAM state hashes with the stored
# references (determinism and behaviour after a change). Recordings and references stay local (they derive from
# the game and your saves): <dir>\NAME.txt from --record, with NAME.state next to it.
# The hashes cover user RAM, which includes the buffers the game mixes audio into: an intended change to audio
# output (or to anything else the game writes to RAM) changes them too; rewrite the references with -Update then.
#
#   scripts\regress.ps1                  # check every recording that has a reference
#   scripts\regress.ps1 -Update          # (re)write the references from the current build
#   scripts\regress.ps1 -Dir D:\rec -Exe build\windows-x64-debug\ff7cc.exe
param(
    [string]$Dir = 'data\recordings',
    [string]$Exe = 'build\windows-x64-release\ff7cc.exe',
    [string]$GameDir = 'data\original',
    [string]$CookedDir = 'data\cooked',  # movie frames reach the game's RAM: replays need the same cooked media
    [int]$Every = 500,   # vblanks between hashes
    [switch]$Update
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Test-Path $Exe)) { throw "$Exe not found; build first (scripts\build.ps1 -Preset windows-x64-release)" }
    $recordings = Get-ChildItem $Dir -Filter *.txt | Where-Object {
        (Get-Content $_.FullName -TotalCount 1) -match '^(psprecore|ff7cc)-input '
    }
    if (-not $Update) { $recordings = $recordings | Where-Object { Test-Path ([IO.Path]::ChangeExtension($_.FullName, '.state')) } }
    if (-not $recordings) { throw "no recordings$(if (-not $Update) { ' with references (run with -Update first)' }) in $Dir" }

    # One headless replay per recording, in parallel.
    $jobs = foreach ($r in $recordings) {
        $last = (Get-Content $r.FullName -Tail 1).Split(' ')[0]
        Start-Job -ArgumentList $root, $Exe, $GameDir, $CookedDir, $r.FullName, $last, $Every -ScriptBlock {
            param($root, $exe, $game, $cooked, $rec, $last, $every)
            Set-Location $root
            $ErrorActionPreference = 'Continue'
            $out = & $exe $game --headless --cooked $cooked --replay $rec --vblanks $last --state-hash $every 2>&1
            [pscustomobject]@{ Recording = $rec; Hashes = @($out | ForEach-Object { "$_" } | ForEach-Object { if ($_ -match 'state vblank .*') { $Matches[0] } }) }
        }
    }
    $results = $jobs | Wait-Job | Receive-Job
    $jobs | Remove-Job

    $failed = 0
    foreach ($res in $results | Sort-Object Recording) {
        $name = [IO.Path]::GetFileNameWithoutExtension($res.Recording)
        $ref = [IO.Path]::ChangeExtension($res.Recording, '.state')
        if (-not $res.Hashes) { Write-Host "FAIL  $name (no state hashes; the replay did not run)"; $failed++; continue }
        if ($Update) {
            Set-Content -Path $ref -Value $res.Hashes -Encoding ascii
            Write-Host "wrote $name ($($res.Hashes.Count) hashes)"
            continue
        }
        $expected = @(Get-Content $ref)
        $diff = $null
        for ($i = 0; $i -lt [Math]::Max($expected.Count, $res.Hashes.Count); $i++) {
            if ($expected[$i] -ne $res.Hashes[$i]) { $diff = "expected '$($expected[$i])', got '$($res.Hashes[$i])'"; break }
        }
        if ($diff) { Write-Host "FAIL  $name  first difference: $diff"; $failed++ }
        else { Write-Host "ok    $name ($($expected.Count) hashes)" }
    }
    if ($failed) { throw "$failed recording(s) differ" }
} finally {
    Pop-Location
}
