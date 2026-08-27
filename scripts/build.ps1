param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build\msvc-x64"
$installRoot = Join-Path $projectRoot "dist\RetroBurner"

function Find-CMakeVisualStudioGenerator {
    param(
        [Parameter(Mandatory = $true)]
        [int]$VisualStudioMajor
    )

    $pattern = "Visual Studio\s+$VisualStudioMajor\s+\d{4}"
    foreach ($line in (& cmake --help 2>&1)) {
        $match = [regex]::Match($line, $pattern)
        if ($match.Success) {
            return $match.Value
        }
    }

    return $null
}

function Show-CMakeFailureDetails {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory
    )

    $logPaths = @(
        (Join-Path $BuildDirectory "CMakeFiles\CMakeConfigureLog.yaml"),
        (Join-Path $BuildDirectory "CMakeFiles\CMakeError.log")
    )

    foreach ($logPath in $logPaths) {
        if (Test-Path $logPath) {
            Write-Host ""
            Write-Host "Last 80 lines from $logPath"
            Write-Host ""
            Get-Content -LiteralPath $logPath -Tail 80
            return
        }
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake was not found on PATH. Install the Windows x64 CMake distribution."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Microsoft C++ Build Tools were not found. Install Build Tools 2022 with the C++ x64 tools and Windows SDK."
}

$installationJson = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -format json -utf8
if ($LASTEXITCODE -ne 0) {
    throw "vswhere could not inspect the installed Microsoft C++ Build Tools."
}

$installations = @($installationJson | ConvertFrom-Json)
if ($installations.Count -eq 0) {
    throw "The MSVC x64 compiler component is missing from Microsoft C++ Build Tools."
}

$installation = $installations[0]
$visualStudioPath = [string]$installation.installationPath
$visualStudioVersion = [version]$installation.installationVersion
$visualStudioMajor = $visualStudioVersion.Major
$visualStudioName = [string]$installation.displayName

$compilerCandidates = @(Get-ChildItem `
    -Path (Join-Path $visualStudioPath "VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe") `
    -File -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)
if ($compilerCandidates.Count -eq 0) {
    throw "Visual Studio reports that MSVC is installed, but cl.exe is missing. Open Visual Studio Installer, choose Modify, and repair the MSVC x64/x86 build tools component."
}

$compilerPath = $compilerCandidates[0].FullName
$compilerDirectory = Split-Path -Parent $compilerPath
$linkerPath = Join-Path $compilerDirectory "link.exe"
$msbuildPath = Join-Path $visualStudioPath "MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $linkerPath)) {
    throw "MSVC cl.exe was found, but link.exe is missing from the same toolset. Repair the Microsoft C++ Build Tools installation."
}
if (-not (Test-Path $msbuildPath)) {
    throw "MSVC was found, but MSBuild.exe is missing. Repair the Microsoft C++ Build Tools installation."
}

$generator = Find-CMakeVisualStudioGenerator -VisualStudioMajor $visualStudioMajor
if ([string]::IsNullOrWhiteSpace($generator)) {
    throw "This CMake version does not support $visualStudioName (major version $visualStudioMajor). Install the current Windows x64 release of CMake, then run this build again."
}

& (Join-Path $PSScriptRoot "bootstrap-deps.ps1")

# RetroBeam is RetroBurner's recording backend. Build/stage it before MSVC
# configures the GUI so the exact current backend is embedded in RetroBurner.exe.
$retroBeamBuild = Join-Path $PSScriptRoot "internal\build-retrobeam-win.ps1"
if (-not (Test-Path -LiteralPath $retroBeamBuild)) {
    throw "RetroBeam build script is missing: $retroBeamBuild"
}
& $retroBeamBuild -NoVersionCheck
if ($LASTEXITCODE -ne 0) {
    throw "RetroBeam build/stage failed."
}
$retroBeamExe = Join-Path $projectRoot "tools\retrobeam-dev\retrobeam.exe"
if (-not (Test-Path -LiteralPath $retroBeamExe)) {
    throw "RetroBeam build completed without staging: $retroBeamExe"
}
Write-Host "RetroBeam backend: $retroBeamExe"

Write-Host "Using: $visualStudioName $($installation.catalog.productDisplayVersion)"
Write-Host "CMake generator: $generator"
Write-Host "Compiler: $compilerPath"

Push-Location $projectRoot
try {
    & cmake -S $projectRoot -B $buildRoot `
        -G $generator -A x64 `
        "-DCMAKE_GENERATOR_INSTANCE=$visualStudioPath" `
        "-DCMAKE_INSTALL_PREFIX:PATH=$installRoot"
    if ($LASTEXITCODE -ne 0) {
        Show-CMakeFailureDetails -BuildDirectory $buildRoot
        throw "CMake could not configure with the validated MSVC installation. The detailed compiler test is printed above."
    }

    & cmake --build $buildRoot --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "The $Configuration build failed."
    }
}
finally {
    Pop-Location
}

$binary = Join-Path $buildRoot "$Configuration\RetroBurner.exe"
if (-not (Test-Path $binary)) {
    throw "The build completed without producing the expected executable: $binary"
}

Write-Host ""
Write-Host "Built: $binary"
