param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build\msvc-x64"
$buildDriver = Join-Path $projectRoot "scripts\build.ps1"

if (-not $IsWindows -and $PSVersionTable.PSEdition -eq "Core") {
    throw "build-windows.ps1 must be run natively on Windows."
}

if (-not (Test-Path -LiteralPath $buildDriver)) {
    throw "Internal Windows build driver is missing: $buildDriver"
}

Write-Host "============================================================"
Write-Host " Retro Burner - Native Windows Build"
Write-Host " Configuration: $Configuration"
Write-Host "============================================================"
Write-Host ""

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    Write-Host "Cleaning: $buildRoot"
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}

& $buildDriver -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$binary = Join-Path $buildRoot "$Configuration\RetroBurner.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Windows build completed without RetroBurner.exe: $binary"
}

Write-Host ""
Write-Host "[PASS] Native Windows Retro Burner built successfully."
Write-Host "Binary: $binary"
