param(
    [switch]$Clean,
    [switch]$NoStage,
    [switch]$NoVersionCheck
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = [IO.Path]::GetFullPath($Root)

$SourceDir = Join-Path $Root "cmake\retroburner-optical"
$BuildDir  = Join-Path $Root "build\retrobeam-optical-mingw32"
$StageDir  = Join-Path $Root "tools\retrobeam-dev"
$StageExe  = Join-Path $StageDir "retrobeam.exe"

$Gcc = "C:\msys64\mingw32\bin\gcc.exe"

# RB_SHA256_FALLBACK_V84B
function Get-Sha256Hex([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $bytes = $sha.ComputeHash($stream)
        return ([BitConverter]::ToString($bytes)).Replace("-", "")
    }
    finally {
        if ($stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Find-Tool([string]$Name,[string[]]$Candidates) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    return $null
}

$CMake = Find-Tool "cmake" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\msys64\mingw32\bin\cmake.exe"
)

$Ninja = Find-Tool "ninja" @(
    "C:\msys64\mingw32\bin\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

foreach ($required in @($SourceDir,$Gcc,$CMake,$Ninja)) {
    if (-not $required -or -not (Test-Path -LiteralPath $required)) {
        throw "Required RetroBeam build dependency missing: $required"
    }
}

$oldPath = $env:PATH
$oldCC = $env:CC
$oldRC = $env:RC
$oldMSYSTEM = $env:MSYSTEM
$oldCHERE = $env:CHERE_INVOKING
$oldPathType = $env:MSYS2_PATH_TYPE

try {
    $env:PATH = "C:\msys64\mingw32\bin;C:\msys64\usr\bin;$oldPath"
    $env:CC = "gcc.exe"
    $env:RC = "windres.exe"
    $env:MSYSTEM = "MINGW32"
    $env:CHERE_INVOKING = "1"
    $env:MSYS2_PATH_TYPE = "inherit"

    $target = (& $Gcc -dumpmachine 2>&1 | Out-String).Trim()
    $version = (& $Gcc -dumpfullversion 2>&1 | Out-String).Trim()

    if ($target -ne "i686-w64-mingw32") {
        throw "Wrong GCC target: $target"
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "RetroBeam Windows build"
    Write-Host "============================================================"
    Write-Host "GCC: $target / $version"
    Write-Host ""

    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Write-Host "[INFO] cleaning:"
        Write-Host "       $BuildDir"
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }

    & $CMake `
        -S $SourceDir `
        -B $BuildDir `
        -G "Ninja" `
        "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        "-DCMAKE_BUILD_TYPE=Release"

    if ($LASTEXITCODE -ne 0) {
        throw "RetroBeam CMake configure failed."
    }

    & $CMake --build $BuildDir --parallel

    if ($LASTEXITCODE -ne 0) {
        throw "RetroBeam build failed."
    }

    $exe = Join-Path $BuildDir "bin\retrobeam.exe"

    if (-not (Test-Path -LiteralPath $exe)) {
        $exe = Get-ChildItem `
            -LiteralPath $BuildDir `
            -Filter "retrobeam.exe" `
            -Recurse -File |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }

    if (-not $exe -or -not (Test-Path -LiteralPath $exe)) {
        throw "Build completed but retrobeam.exe was not found."
    }

    $hash = Get-Sha256Hex $exe

    Write-Host ""
    Write-Host "[PASS] built:"
    Write-Host "       $exe"
    Write-Host "       SHA256 $hash"

    if (-not $NoStage) {
        New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
        Copy-Item -LiteralPath $exe -Destination $StageExe -Force

        foreach ($dll in @(
            "C:\msys64\mingw32\bin\libintl-8.dll",
            "C:\msys64\mingw32\bin\libiconv-2.dll",
            "C:\msys64\mingw32\bin\libwinpthread-1.dll",
            "C:\msys64\mingw32\bin\libgcc_s_dw2-1.dll"
        )) {
            if (Test-Path -LiteralPath $dll) {
                Copy-Item -LiteralPath $dll -Destination $StageDir -Force
            }
        }

        $stageHash = Get-Sha256Hex $StageExe

        if ($stageHash -ne $hash) {
            throw "Staged RetroBeam binary hash mismatch."
        }

        Write-Host "[PASS] staged:"
        Write-Host "       $StageExe"
    }

    if (-not $NoVersionCheck) {
        Write-Host ""
        Write-Host "Version check (read only):"
        & $exe -version

        if ($LASTEXITCODE -ne 0) {
            throw "RetroBeam -version returned exit code $LASTEXITCODE."
        }
    }

    Write-Host ""
    Write-Host "[PASS] RetroBeam Windows build complete"
}
finally {
    $env:PATH = $oldPath

    if ($null -eq $oldCC) { Remove-Item Env:CC -ErrorAction SilentlyContinue }
    else { $env:CC = $oldCC }

    if ($null -eq $oldRC) { Remove-Item Env:RC -ErrorAction SilentlyContinue }
    else { $env:RC = $oldRC }

    if ($null -eq $oldMSYSTEM) { Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue }
    else { $env:MSYSTEM = $oldMSYSTEM }

    if ($null -eq $oldCHERE) { Remove-Item Env:CHERE_INVOKING -ErrorAction SilentlyContinue }
    else { $env:CHERE_INVOKING = $oldCHERE }

    if ($null -eq $oldPathType) { Remove-Item Env:MSYS2_PATH_TYPE -ErrorAction SilentlyContinue }
    else { $env:MSYS2_PATH_TYPE = $oldPathType }
}