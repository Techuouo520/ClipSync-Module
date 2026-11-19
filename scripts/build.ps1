# Build host tests, arm64 clipsyncd, clip.dex, and build/clipsync-magisk.zip.
$ErrorActionPreference = 'Stop'

function Require-EnvVar([string]$Name) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "environment variable $Name is not set"
    }
    if (-not (Test-Path -LiteralPath $value)) {
        throw "$Name does not exist: $value"
    }
    return $value
}

if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    throw "run this file with powershell -File"
}
$Root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $Root

$Ndk = Require-EnvVar "ANDROID_NDK_HOME"
$JavaHome = Require-EnvVar "JAVA_HOME"
$SdkName = "ANDROID_HOME"
$Sdk = [Environment]::GetEnvironmentVariable("ANDROID_HOME")
if ([string]::IsNullOrWhiteSpace($Sdk)) {
    $SdkName = "ANDROID_SDK_ROOT"
    $Sdk = [Environment]::GetEnvironmentVariable("ANDROID_SDK_ROOT")
}
if ([string]::IsNullOrWhiteSpace($Sdk)) {
    throw "environment variable ANDROID_HOME is not set"
}
if (-not (Test-Path -LiteralPath $Sdk)) {
    throw "$SdkName does not exist: $Sdk"
}

$Clang = Get-Command clang++ -ErrorAction SilentlyContinue
if (-not $Clang) {
    throw "clang++ is not on PATH"
}
$Toolchain = Join-Path $Ndk "build\cmake\android.toolchain.cmake"
if (-not (Test-Path -LiteralPath $Toolchain)) {
    throw "android.toolchain.cmake not found under ANDROID_NDK_HOME"
}

$HostBuild = Join-Path $Root "build\host"
& cmake -S $Root -B $HostBuild -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_COMPILER=$($Clang.Source)"
if ($LASTEXITCODE -ne 0) { throw "host cmake configure failed" }
& cmake --build $HostBuild
if ($LASTEXITCODE -ne 0) { throw "host build failed" }
$TestExe = Join-Path $HostBuild "clipsync_tests.exe"
if (-not (Test-Path -LiteralPath $TestExe)) {
    $TestExe = Join-Path $HostBuild "clipsync_tests"
}
& $TestExe
if ($LASTEXITCODE -ne 0) { throw "host tests failed" }

$AndroidBuild = Join-Path $Root "build\android"
& cmake --preset release
if ($LASTEXITCODE -ne 0) { throw "android cmake configure failed" }
& cmake --build --preset release
if ($LASTEXITCODE -ne 0) { throw "android build failed" }
$Daemon = Join-Path $AndroidBuild "clipsyncd"
if (-not (Test-Path -LiteralPath $Daemon)) { throw "clipsyncd was not produced" }

$BestApi = -1
$Jar = $null
foreach ($dir in (Get-ChildItem -LiteralPath (Join-Path $Sdk "platforms") -Directory)) {
    if ($dir.Name -match '^android-(\d+)') {
        $api = [int]$Matches[1]
        $candidate = Join-Path $dir.FullName "android.jar"
        if ($api -gt $BestApi -and (Test-Path -LiteralPath $candidate)) {
            $BestApi = $api
            $Jar = $candidate
        }
    }
}
if (-not $Jar) { throw "android.jar not found under ANDROID_HOME/platforms" }

$BestVer = [version]"0.0.0"
$D8 = $null
foreach ($dir in (Get-ChildItem -LiteralPath (Join-Path $Sdk "build-tools") -Directory)) {
    try { $ver = [version]$dir.Name } catch { continue }
    $candidate = Join-Path $dir.FullName "d8.bat"
    if ($ver -gt $BestVer -and (Test-Path -LiteralPath $candidate)) {
        $BestVer = $ver
        $D8 = $candidate
    }
}
if (-not $D8) { throw "d8.bat not found under ANDROID_HOME/build-tools" }

$DexOut = Join-Path $Root "build\dex"
$ClassOut = Join-Path $DexOut "classes"
if (Test-Path -LiteralPath $ClassOut) { Remove-Item -LiteralPath $ClassOut -Recurse -Force }
New-Item -ItemType Directory -Force -Path $ClassOut | Out-Null
$Javac = Join-Path $JavaHome "bin\javac.exe"
$Sources = @(Get-ChildItem -LiteralPath (Join-Path $Root "dex") -Filter *.java | ForEach-Object { $_.FullName })
& $Javac --release 8 -encoding UTF-8 -g:none -classpath $Jar -d $ClassOut @Sources
if ($LASTEXITCODE -ne 0) { throw "javac failed" }
$Classes = @(Get-ChildItem -LiteralPath $ClassOut -Recurse -Filter *.class | ForEach-Object { $_.FullName })
& $D8 --release --lib $Jar --output $DexOut @Classes
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }
$Dex = Join-Path $DexOut "classes.dex"
$DexInfo = Get-Item -LiteralPath $Dex
if ($DexInfo.Length -ge 8192) { throw "clip.dex is $($DexInfo.Length) bytes" }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$ZipPath = Join-Path $Root "build\clipsync-magisk.zip"
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
$zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    function Add-ZipEntry([System.IO.Compression.ZipArchive]$archive, [string]$path, [string]$entry) {
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $path, $entry)
    }
    Add-ZipEntry $zip (Join-Path $Root "module\module.prop") "module.prop"
    Add-ZipEntry $zip (Join-Path $Root "LICENSE") "LICENSE"
    Add-ZipEntry $zip (Join-Path $Root "README.md") "README.md"
    foreach ($entry in @("docs/en/README.md", "docs/assets/sponsor-qr.jpg")) {
        Add-ZipEntry $zip (Join-Path $Root $entry) $entry
    }
    Add-ZipEntry $zip (Join-Path $Root "module\customize.sh") "customize.sh"
    Add-ZipEntry $zip (Join-Path $Root "module\service.sh") "service.sh"
    Add-ZipEntry $zip (Join-Path $Root "module\action.sh") "action.sh"
    Add-ZipEntry $zip (Join-Path $Root "module\webui.sh") "webui.sh"
    $WebRoot = Join-Path $Root "module\webroot"
    foreach ($file in (Get-ChildItem -LiteralPath $WebRoot -File -Recurse)) {
        $relative = $file.FullName.Substring($WebRoot.Length + 1).Replace('\', '/')
        Add-ZipEntry $zip $file.FullName "webroot/$relative"
    }
    Add-ZipEntry $zip (Join-Path $Root "module\META-INF\com\google\android\update-binary") "META-INF/com/google/android/update-binary"
    Add-ZipEntry $zip (Join-Path $Root "module\META-INF\com\google\android\updater-script") "META-INF/com/google/android/updater-script"
    Add-ZipEntry $zip $Daemon "system/bin/clipsyncd"
    Add-ZipEntry $zip $Dex "system/framework/clip.dex"
} finally {
    $zip.Dispose()
}
Write-Output "zip=$ZipPath"
Write-Output "dexBytes=$($DexInfo.Length)"
Write-Output "daemon=$Daemon"
