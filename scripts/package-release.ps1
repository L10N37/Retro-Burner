$ErrorActionPreference = "Stop"

$version = "0.5.0"
$projectRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $projectRoot "dist"
$packageName = "RetroBurner-$version-windows-x64"
$packageRoot = Join-Path $distRoot $packageName
$zipPath = Join-Path $distRoot "$packageName.zip"
$hashPath = "$zipPath.sha256"

& (Join-Path $projectRoot "build-windows.ps1") -Configuration Release -Clean
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$binary = Join-Path $projectRoot "build\msvc-x64\Release\RetroBurner.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Release build did not produce: $binary"
}

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

Copy-Item -LiteralPath $binary -Destination (Join-Path $packageRoot "RetroBurner.exe")
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "RELEASE_NOTES_0.5.0.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "licenses") -Destination $packageRoot -Recurse

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $hashPath) {
    Remove-Item -LiteralPath $hashPath -Force
}

Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zipPath))" |
    Set-Content -LiteralPath $hashPath -Encoding ascii

Write-Host ""
Write-Host "[PASS] Windows release package ready"
Write-Host "Archive: $zipPath"
Write-Host "SHA256:  $hash"
Write-Host "Hash:    $hashPath"
