# Builds the Android APK: the native library with CMake (preset android-arm64-release), then the app with Gradle.
#
#   scripts\build-android.ps1              # -> android\app\build\outputs\apk\release\app-release.apk
#   scripts\build-android.ps1 -Install     # also installs it on the connected device (adb)
#   scripts\build-android.ps1 -Install -Device 57211FDCH003WQ   # on this device (serial or model, see adb devices -l)
#   scripts\build-android.ps1 -ListDevices # shows the connected devices and exits
#
# With several devices connected and no -Device (or ANDROID_SERIAL), -Install asks which one to use.
#
# Needs: Android SDK (ANDROID_HOME) with NDK 29.0.14206865, JDK 17+, the Vulkan SDK (VULKAN_SDK: slangc, volk, VMA),
# and generated/ from ccrecomp. CMake/Ninja are taken from Visual Studio when not on PATH.
param(
    [switch]$Install,
    [string]$Device,      # adb serial, or a model name such as Pixel_10_Pro (as adb devices -l prints it)
    [switch]$ListDevices
)
$ErrorActionPreference = 'Continue'  # native tools write warnings to stderr; exit codes are checked
$root = Split-Path -Parent $PSScriptRoot

if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'

# Connected devices as objects: Serial, Model.
function Get-Devices {
    & $adb devices -l | Select-Object -Skip 1 | Where-Object { $_ -match '^(\S+)\s+device\b(.*)$' } | ForEach-Object {
        $model = if ($Matches[2] -match 'model:(\S+)') { $Matches[1] } else { '' }
        [pscustomobject]@{ Serial = ($_ -split '\s+')[0]; Model = $model }
    }
}

# Resolves the install target: -Device (serial or model), then ANDROID_SERIAL, then the only device, else a prompt.
function Select-Device {
    $devices = @(Get-Devices)
    if ($devices.Count -eq 0) { throw 'No Android device is connected (adb devices).' }
    if ($Device) {
        $match = @($devices | Where-Object { $_.Serial -eq $Device -or $_.Model -eq $Device -or $_.Model -eq ($Device -replace ' ', '_') })
        if ($match.Count -ne 1) { throw "Device '$Device' not found (or ambiguous). Connected: $(($devices | ForEach-Object { "$($_.Serial) ($($_.Model))" }) -join ', ')" }
        return $match[0].Serial
    }
    if ($env:ANDROID_SERIAL) { return $env:ANDROID_SERIAL }
    if ($devices.Count -eq 1) { return $devices[0].Serial }
    if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        for ($i = 0; $i -lt $devices.Count; ++$i) { Write-Host "  [$($i + 1)] $($devices[$i].Serial)  $($devices[$i].Model)" }
        $choice = Read-Host "Install on which device? (1-$($devices.Count))"
        $n = 0
        if ([int]::TryParse($choice, [ref]$n) -and $n -ge 1 -and $n -le $devices.Count) { return $devices[$n - 1].Serial }
        throw 'No device selected.'
    }
    throw "Several devices are connected; pass -Device <serial or model>: $(($devices | ForEach-Object { "$($_.Serial) ($($_.Model))" }) -join ', ')"
}

if ($ListDevices) {
    Get-Devices | Format-Table -AutoSize
    return
}
$target = if ($Install) { Select-Device } else { $null }  # before the long build, so a wrong choice fails fast
$ndk = Join-Path $env:ANDROID_HOME 'ndk\29.0.14206865'
if (-not (Test-Path $ndk)) { throw "NDK 29.0.14206865 not found under $env:ANDROID_HOME\ndk" }
$env:ANDROID_NDK_HOME = $ndk

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -property installationPath
    $env:PATH = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
}

Push-Location $root
try {
    cmake --preset android-arm64-release
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    cmake --build --preset android-arm64-release
    if ($LASTEXITCODE) { throw 'CMake build failed' }

    $jni = Join-Path $root 'build\android-arm64-release\jniLibs\arm64-v8a'
    New-Item -ItemType Directory -Force $jni | Out-Null
    Copy-Item (Join-Path $root 'build\android-arm64-release\runtime\libmain.so') $jni -Force

    Push-Location (Join-Path $root 'android')
    try {
        .\gradlew.bat assembleRelease --console=plain
        if ($LASTEXITCODE) { throw 'Gradle build failed' }
    } finally { Pop-Location }

    $apk = Join-Path $root 'android\app\build\outputs\apk\release\app-release.apk'
    Write-Host "APK: $apk"
    if ($Install) {
        Write-Host "Installing on $target"
        & $adb -s $target install -r $apk
        if ($LASTEXITCODE) { throw 'adb install failed' }
    }
} finally { Pop-Location }
