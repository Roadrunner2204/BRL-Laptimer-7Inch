#!/usr/bin/env pwsh
# BRL Telemetry — One-command Debug-APK build & install.
# Run from android-app/ :   .\build.ps1
#
# Flags:
#   -Clean     : gradlew clean before assemble
#   -SkipInstall : build only, no adb install

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipInstall
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $root

Write-Host "==> Bundling JS (expo export:embed)" -ForegroundColor Cyan
$bundleOut = 'android\app\src\main\assets\index.android.bundle'
$assetsDest = 'android\app\src\main\res'

# Ensure asset dir exists (fresh checkouts)
New-Item -ItemType Directory -Force -Path (Split-Path $bundleOut -Parent) | Out-Null
New-Item -ItemType Directory -Force -Path $assetsDest | Out-Null

npx expo export:embed `
    --platform android `
    --dev true `
    --entry-file index.js `
    --bundle-output $bundleOut `
    --assets-dest $assetsDest
if ($LASTEXITCODE -ne 0) { throw "expo export:embed failed" }

Set-Location (Join-Path $root 'android')

if ($Clean) {
    Write-Host "==> gradlew clean" -ForegroundColor Cyan
    .\gradlew clean
    if ($LASTEXITCODE -ne 0) { throw "gradle clean failed" }
}

Write-Host "==> gradlew assembleDebug" -ForegroundColor Cyan
.\gradlew assembleDebug
if ($LASTEXITCODE -ne 0) { throw "gradle assembleDebug failed" }

$apk = 'app\build\outputs\apk\debug\app-debug.apk'
Write-Host "==> APK ready: $apk" -ForegroundColor Green

if (-not $SkipInstall) {
    Write-Host "==> adb install -r" -ForegroundColor Cyan
    adb install -r $apk
    if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
    Write-Host "==> Installed on device" -ForegroundColor Green
}

Set-Location $root
