param(
    [switch]$Configure,
    [string]$Target = 'LuigisMansionRecomp'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$llvmBin = Join-Path $projectRoot '.tools\llvm\bin'
$clang = Join-Path $llvmBin 'clang-cl.exe'
$headers = Join-Path $projectRoot '.tools\llvm\lib\clang\22\include\emmintrin.h'
if (-not (Test-Path -LiteralPath $clang) -or -not (Test-Path -LiteralPath $headers)) {
    throw 'The local LLVM toolchain is missing. See README-VR-BASE.md.'
}
$previousPath = $env:PATH
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $ninjaBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $cmakeBin = Join-Path $env:ProgramFiles 'CMake\bin'
    $env:PATH = "$llvmBin;$ninjaBin;$cmakeBin;$env:PATH"
    $arguments = @{ Target = $Target; Preset = 'windows-vr-integration' }
    if ($Configure) { $arguments.Configure = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @arguments
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally {
    $env:PATH = $previousPath
}
