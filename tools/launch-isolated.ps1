param(
    [ValidateSet('base', 'vr', 'first-person')][string]$Profile = 'vr',
    [string]$Executable,
    [switch]$EnableVR,
    [string]$OpenXRLoader = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openxr_loader.dll',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) {
    $relativeExe = if ($Profile -eq 'base') { 'base-game\LuigisMansionRecomp.exe' } elseif ($Profile -eq 'first-person') { 'out\first-person\LuigisMansionRecomp.exe' } else { 'out\vr\LuigisMansionRecomp.exe' }
    $Executable = Join-Path $projectRoot $relativeExe
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$profileRoot = Join-Path $projectRoot "profiles\$Profile"
$environment = @{
    APPDATA = Join-Path $profileRoot 'appdata'
    LM_CARD_DIR = Join-Path $profileRoot 'cards'
    LM_CACHE_DIR = Join-Path $profileRoot 'cache'
}
if ($Profile -eq 'first-person') { $EnableVR = $true; $environment.LM_VR_FIRST_PERSON = '1' }
if ($EnableVR) {
    if ($Profile -eq 'base') { throw 'The frozen base build is not a VR executable.' }
    $environment.LM_VR = '1'
    $environment.LM_OPENXR_LOADER = (Resolve-Path -LiteralPath $OpenXRLoader).Path
}
if ($CheckOnly) {
    [pscustomobject]@{ Executable = $Executable; Profile = $profileRoot; Environment = $environment }
    return
}
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $Executable
$startInfo.WorkingDirectory = Split-Path -Parent $Executable
$startInfo.UseShellExecute = $false
foreach ($entry in $environment.GetEnumerator()) {
    if ($entry.Key -in @('APPDATA','LM_CARD_DIR','LM_CACHE_DIR')) { New-Item -ItemType Directory -Force -Path $entry.Value | Out-Null }
    $startInfo.EnvironmentVariables[$entry.Key] = $entry.Value
}
$process = [System.Diagnostics.Process]::Start($startInfo)
Write-Host "Started $Profile profile (PID $($process.Id))."
