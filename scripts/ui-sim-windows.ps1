param(
    [ValidateSet(
        "all",
        "dreamcast",
        "ps1",
        "ps2cd",
        "ps2cd-verify",
        "ps2dvd-retrobeam",
        "ps2dvd-growisofs",
        "saturn",
        "xgd2-retrobeam",
        "xgd2-growisofs",
        "xgd3-retrobeam",
        "xgd3-growisofs",
        "ps2cd-failure"
    )]
    [string]$Scenario = "all"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Root "build\msvc-x64\Release\RetroBurner.exe"

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Build Windows Release first: .\build-windows.ps1 -Configuration Release"
}

$oldValue = $env:RETROBURNER_UI_SIM

try {
    $env:RETROBURNER_UI_SIM = $Scenario

    Write-Host "Retro Burner UI simulation: $Scenario"
    Write-Host "NO optical backend is started. NO disc WRITE command is issued."
    Write-Host "Close the app when inspection is complete."

    & $Exe
}
finally {
    if ($null -eq $oldValue) {
        Remove-Item Env:RETROBURNER_UI_SIM -ErrorAction SilentlyContinue
    } else {
        $env:RETROBURNER_UI_SIM = $oldValue
    }
}