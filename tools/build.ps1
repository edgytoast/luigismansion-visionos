# Configure and build from a plain PowerShell by importing the MSVC developer environment.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Preset windows-clang] [-Configure]
param(
    [string]$Preset = "windows-clang",
    [switch]$Configure,
    [string]$Target = "",
    [string]$Dol = ""  # sets LM_DOL (the main.dol to recompile) when configuring
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio C++ tools not found" }

# Import vcvars64 into this process.
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
# CMake/Ninja installed by winget may not be on PATH in older shells.
foreach ($p in @("$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")) {
    if (Test-Path $p) { $env:PATH = "$p;$env:PATH" }
}

Push-Location $root
try {
    $buildRoot = if ($Preset -eq "windows-vr-integration") { "build/windows-clang" } else { "build/$Preset" }
    if ($Configure -or $Dol -or -not (Test-Path "$buildRoot/build.ninja")) {
        if ($Dol) { cmake --preset $Preset "-DLM_DOL=$($Dol -replace '\\', '/')" } else { cmake --preset $Preset }
        if ($LASTEXITCODE) { throw "configure failed" }
    }
    if ($Target) { cmake --build --preset $Preset --target $Target } else { cmake --build --preset $Preset }
    if ($LASTEXITCODE) { throw "build failed" }
} finally {
    Pop-Location
}
