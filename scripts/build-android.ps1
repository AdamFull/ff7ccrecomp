# Builds the Android APK: the native library with CMake (preset android-arm64-release), then the app with Gradle.
#
#   scripts\build-android.ps1              # -> android\app\build\outputs\apk\release\app-release.apk
#   scripts\build-android.ps1 -Install     # also installs it on the connected device (adb)
#
# Needs: Android SDK (ANDROID_HOME) with NDK 29.0.14206865, JDK 17+, the Vulkan SDK (VULKAN_SDK: slangc, volk, VMA),
# and generated/ from ccrecomp. CMake/Ninja are taken from Visual Studio when not on PATH.
param(
    [switch]$Install
)
$ErrorActionPreference = 'Continue'  # native tools write warnings to stderr; exit codes are checked
$root = Split-Path -Parent $PSScriptRoot

if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
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
        & (Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe') install -r $apk
        if ($LASTEXITCODE) { throw 'adb install failed' }
    }
} finally { Pop-Location }
