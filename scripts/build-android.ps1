# Builds the Android APK and optionally installs it and the game data on a device.
#
#   scripts\build-android.ps1                         # -> android\app\build\outputs\apk\release\app-release.apk
#   scripts\build-android.ps1 -Install                # also installs the APK (adb)
#   scripts\build-android.ps1 -Install -InstallData   # ... and copies the game folder and cooked/ to the device
#   scripts\build-android.ps1 -InstallData            # only copies the data (no build); the app must be installed
#   scripts\build-android.ps1 -Install -Device Pixel_10_Pro   # pick the device: adb serial or model (adb devices -l)
#   scripts\build-android.ps1 -ListDevices            # shows the connected devices and exits
#   scripts\build-android.ps1 -Install -Smoke         # ... then starts the app silently for 2 seconds of game time
#                                                     # and checks that it got there (library loads, game boots)
#
# With several devices connected and no -Device (or ANDROID_SERIAL), the script asks which one to use.
#
# Game data goes to Android/data/com.ff7ccrecomp/files/ ("Crisis Core - Final Fantasy VII EU/PSP_GAME/..." and
# "cooked/"). Copies are incremental (adb push --sync). The app must create that folder itself, so -InstallData starts
# it once if needed: a folder created earlier by adb belongs to the shell user and the app cannot read it.
# -ResetAppData repairs that by uninstalling the app first, which DELETES its files folder including saves (it needs
# -Install to put the app back).
#
# Needs: Android SDK (ANDROID_HOME) with NDK 29.0.14206865, JDK 17+, the Vulkan SDK (VULKAN_SDK: slangc, volk, VMA),
# and generated/ from psprecomp. CMake/Ninja are taken from Visual Studio when not on PATH.
param(
    [switch]$Install,
    [switch]$InstallData,
    [string]$Device,        # adb serial, or a model name such as Pixel_10_Pro (as adb devices -l prints it)
    [string]$GameDir,       # default: "<repo>\Crisis Core - Final Fantasy VII EU"
    [string]$CookedDir,     # default: "<repo>\cooked"
    [switch]$ResetAppData,  # uninstall first (deletes the app's files and saves on the device)
    [switch]$SkipBuild,     # install the APK built last time
    [switch]$ListDevices,
    [switch]$Smoke          # launch test after installing (no sound; needs the game data on the device)
)
$ErrorActionPreference = 'Continue'  # native tools write warnings to stderr; exit codes are checked
$root = Split-Path -Parent $PSScriptRoot
$package = 'com.ff7ccrecomp'
$remoteFiles = "/sdcard/Android/data/$package/files"
$gameName = 'Crisis Core - Final Fantasy VII EU'  # the folder name the app looks for
if (-not $GameDir) { $GameDir = Join-Path $root $gameName }
if (-not $CookedDir) { $CookedDir = Join-Path $root 'cooked' }

if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'

# Connected devices as objects: Serial, Model.
function Get-Devices {
    & $adb devices -l | Select-Object -Skip 1 | Where-Object { $_ -match '^(\S+)\s+device\b(.*)$' } | ForEach-Object {
        $model = if ($Matches[2] -match 'model:(\S+)') { $Matches[1] } else { '' }
        [pscustomobject]@{ Serial = ($_ -split '\s+')[0]; Model = $model }
    }
}

# Resolves the target: -Device (serial or model), then ANDROID_SERIAL, then the only device, else a prompt.
function Select-Device {
    $devices = @(Get-Devices)
    if ($devices.Count -eq 0) { throw 'No Android device is connected (adb devices).' }
    $list = ($devices | ForEach-Object { "$($_.Serial) ($($_.Model))" }) -join ', '
    if ($Device) {
        $match = @($devices | Where-Object { $_.Serial -eq $Device -or $_.Model -eq $Device -or $_.Model -eq ($Device -replace ' ', '_') })
        if ($match.Count -ne 1) { throw "Device '$Device' not found (or ambiguous). Connected: $list" }
        return $match[0].Serial
    }
    if ($env:ANDROID_SERIAL) { return $env:ANDROID_SERIAL }
    if ($devices.Count -eq 1) { return $devices[0].Serial }
    if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        for ($i = 0; $i -lt $devices.Count; ++$i) { Write-Host "  [$($i + 1)] $($devices[$i].Serial)  $($devices[$i].Model)" }
        $choice = Read-Host "Which device? (1-$($devices.Count))"
        $n = 0
        if ([int]::TryParse($choice, [ref]$n) -and $n -ge 1 -and $n -le $devices.Count) { return $devices[$n - 1].Serial }
        throw 'No device selected.'
    }
    throw "Several devices are connected; pass -Device <serial or model>: $list"
}

function Invoke-Adb {
    & $adb -s $script:target @args
    if ($LASTEXITCODE) { throw "adb $($args -join ' ') failed" }
}
function Get-AdbShell([string]$command) { (& $adb -s $script:target shell $command 2>$null | Out-String).Trim() }

# Makes sure the app has created its own files folder (starting it once if needed) and that the app owns it.
function Initialize-AppFolder {
    $uid = if ((Get-AdbShell "pm list packages -U $package") -match 'uid:(\d+)') { $Matches[1] } else { $null }
    if (-not $uid) { throw "$package is not installed on $($script:target); add -Install." }
    if (-not (Get-AdbShell "ls -d $remoteFiles 2>/dev/null")) {
        Write-Host 'Starting the app once so that it creates its files folder...'
        Invoke-Adb shell am start -n "$package/.GameActivity" | Out-Null
        for ($i = 0; $i -lt 30 -and -not (Get-AdbShell "ls -d $remoteFiles 2>/dev/null"); ++$i) { Start-Sleep -Milliseconds 500 }
        Invoke-Adb shell am force-stop $package
        if (-not (Get-AdbShell "ls -d $remoteFiles 2>/dev/null")) { throw "The app did not create $remoteFiles." }
    }
    $owner = Get-AdbShell "stat -c %u $remoteFiles"
    if ($owner -and $owner -ne $uid) {
        throw "$remoteFiles belongs to user $owner, not to the app ($uid): it was created before the app's first start, " +
              "so the app cannot read it. Rerun with -Install -ResetAppData (uninstalls the app, deleting its files and saves)."
    }
}

function Copy-GameData {
    if (-not (Test-Path (Join-Path $GameDir 'PSP_GAME\SYSDIR\BOOT.BIN'))) { throw "No game at $GameDir (expected PSP_GAME\SYSDIR\BOOT.BIN)." }
    Initialize-AppFolder
    Write-Host "Copying the game to $remoteFiles/$gameName (unchanged files are skipped)..."
    Invoke-Adb shell "mkdir -p '$remoteFiles/$gameName'"
    Invoke-Adb push --sync (Join-Path $GameDir 'PSP_GAME') "$remoteFiles/$gameName/"
    if (Test-Path $CookedDir) {
        Write-Host "Copying cooked media to $remoteFiles/cooked..."
        Invoke-Adb shell "mkdir -p '$remoteFiles/cooked'"
        Get-ChildItem $CookedDir -Directory | Where-Object { $_.Name -ne 'tmp' } | ForEach-Object {
            Invoke-Adb push --sync $_.FullName "$remoteFiles/cooked/"
        }
    } else {
        Write-Warning "No cooked media at $CookedDir (run cccook); music and movies will be silent or black."
    }
    Write-Host 'Game data installed.'
}

if ($ListDevices) {
    Get-Devices | Format-Table -AutoSize
    return
}
if ($ResetAppData -and -not $Install) { throw '-ResetAppData uninstalls the app; it needs -Install to put it back.' }
$script:target = if ($Install -or $InstallData -or $Smoke) { Select-Device } else { $null }  # before the long build: fail fast
$build = -not $SkipBuild -and ($Install -or -not $InstallData)

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -property installationPath
    $env:PATH = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
}

Push-Location $root
try {
    $apk = Join-Path $root 'android\app\build\outputs\apk\release\app-release.apk'
    if ($build) {
        $ndk = Join-Path $env:ANDROID_HOME 'ndk\29.0.14206865'
        if (-not (Test-Path $ndk)) { throw "NDK 29.0.14206865 not found under $env:ANDROID_HOME\ndk" }
        $env:ANDROID_NDK_HOME = $ndk
        cmake --preset android-arm64-release
        if ($LASTEXITCODE) { throw 'CMake configure failed' }
        cmake --build --preset android-arm64-release
        if ($LASTEXITCODE) { throw 'CMake build failed' }

        $jni = Join-Path $root 'build\android-arm64-release\jniLibs\arm64-v8a'
        New-Item -ItemType Directory -Force $jni | Out-Null
        Copy-Item (Join-Path $root 'build\android-arm64-release\libmain.so') $jni -Force

        Push-Location (Join-Path $root 'android')
        try {
            .\gradlew.bat assembleRelease --console=plain
            if ($LASTEXITCODE) { throw 'Gradle build failed' }
        } finally { Pop-Location }
        Write-Host "APK: $apk"
    }
    if ($Install) {
        if (-not (Test-Path $apk)) { throw "No APK at $apk; build it first." }
        if ($ResetAppData) {
            Write-Warning "Uninstalling $package from $($script:target): its files folder and saves are deleted."
            & $adb -s $script:target uninstall $package | Out-Null  # fine if it was not installed
        }
        Write-Host "Installing on $($script:target)"
        Invoke-Adb install -r $apk
    }
    if ($InstallData) { Copy-GameData }
    if ($Smoke) {
        # The app writes its log to files/ff7cc.log; a library that fails to load never gets there.
        $log = "$remoteFiles/ff7cc.log"
        # Right after an install, Android is still handling the package replacement and may restart a fresh launch
        # without its extras (the arguments); let it settle first.
        if ($Install) { Start-Sleep -Seconds 5 }
        & $adb -s $script:target shell rm -f $log | Out-Null
        & $adb -s $script:target shell am force-stop $package | Out-Null
        & $adb -s $script:target shell "am start -n $package/.GameActivity -e args '--no-audio --vblanks 120'" | Out-Null
        $text = ''
        for ($i = 0; $i -lt 30 -and $text -notmatch 'stopped after'; $i++) {
            Start-Sleep -Seconds 1
            $text = (& $adb -s $script:target shell cat $log 2>$null) -join "`n"
        }
        & $adb -s $script:target shell am force-stop $package | Out-Null
        if ($text -notmatch 'stopped after') {
            $why = (& $adb -s $script:target logcat -d -t 300 2>$null |
                    Select-String 'dlopen failed|FATAL|AndroidRuntime' | Select-Object -First 5) -join "`n"
            throw "Smoke test failed: the app did not run 120 vblanks.`n$text`n$why"
        }
        Write-Host "Smoke test passed: $((($text -split "`n") | Select-String 'stopped after') -join '')"
    }
} finally { Pop-Location }
