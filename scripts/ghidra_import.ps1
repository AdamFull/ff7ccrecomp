# Builds a Ghidra project for BOOT.BIN: relocated image at PPSSPP's load base + all recovered symbols.
#
#   scripts\ghidra_import.ps1 [-Ghidra "C:\Program Files\ghidra_12.1.4_PUBLIC"] [-Language MIPS:LE:32:default]
#
# Needs a debug build (scripts\build.ps1), build\nids.tsv (see README) and a JDK supported by your Ghidra
# (Ghidra 12 requires JDK 21; set JAVA_HOME if several JDKs are installed).
# The default language comes from the ghidra-allegrex extension (Allegrex integer + FPU + VFPU), installed in
# %APPDATA%\ghidra\<ghidra version>\Extensions. Without it, pass -Language MIPS:LE:32:default (stock MIPS
# does not know the VFPU, so functions using vector instructions are only partly disassembled).
param(
    [string]$Ghidra = 'C:\Program Files\ghidra_12.1.4_PUBLIC',
    [string]$Language = 'Allegrex:LE:32:default',
    [string]$Base = '0x08804000'
)
$ErrorActionPreference = 'Continue'  # native tools print progress on stderr; exit codes are checked
$root = Split-Path -Parent $PSScriptRoot
$boot = Join-Path $root 'Crisis Core - Final Fantasy VII EU\PSP_GAME\SYSDIR\BOOT.BIN'
$psprx = Join-Path $root 'build\windows-x64-debug\tools\psprx\psprx.exe'
$out = Join-Path $root 'build\ghidra'
New-Item -ItemType Directory -Force $out | Out-Null

& $psprx image $boot $Base "$out\boot_$Base.bin"
if ($LASTEXITCODE) { throw 'psprx image failed' }
& $psprx symbols $boot (Join-Path $root 'build\nids.tsv') "$out\symbols.tsv" $Base "$out\ghidra_symbols.txt"
if ($LASTEXITCODE) { throw 'psprx symbols failed' }

& (Join-Path $Ghidra 'support\analyzeHeadless.bat') $out FF7CC -overwrite `
    -import "$out\boot_$Base.bin" -loader BinaryLoader -loader-baseAddr $Base -processor $Language `
    -scriptPath (Join-Path $root 'tools\ghidra') -preScript ImportFF7CCSymbols.java "$out\ghidra_symbols.txt"
if ($LASTEXITCODE) { throw "analyzeHeadless failed ($LASTEXITCODE)" }
Write-Host "Ghidra project: $out\FF7CC.gpr"
