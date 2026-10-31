param(
    [ValidateSet('FirstPerson','Diorama','LifeSize')][string]$Mode = 'FirstPerson',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
try {
    $root = $PSScriptRoot
    $exe = Join-Path $root 'bin\LuigisMansionRecomp.exe'
    $disc = Join-Path $root 'GameFiles\LuigisMansion-PAL.iso'
    $loader = $env:LM_OPENXR_LOADER
    if (-not $loader) {
        $steamRoot = 'C:\Program Files (x86)\Steam'
        $steam = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
        if ($steam.SteamPath) { $steamRoot = $steam.SteamPath }
        $loader = Join-Path $steamRoot 'steamapps\common\SteamVR\bin\win64\openxr_loader.dll'
    }
    foreach ($file in @($exe,$disc,$loader)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing: $file" }
    }
    $viewMode = @{FirstPerson='first-person'; Diorama='diorama'; LifeSize='life-size'}[$Mode]
    if ($CheckOnly) { Write-Output "Ready VR launcher verified: $Mode ($viewMode)"; exit 0 }
    $running = Get-Process -Name LuigisMansionRecomp -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe }
    if ($running) { throw 'This VR copy is already running. Use F1 to switch modes, or close it before launching again.' }
    $profile = Join-Path $root 'profile'
    $config = Join-Path $profile "appdata\Luigi's Mansion VR"
    $cards = Join-Path $profile 'cards'
    $cache = Join-Path $profile 'cache'
    $logs = Join-Path $profile 'logs'
    foreach ($directory in @($config,$cards,$cache,$logs)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
    $install = "[install]`nus_disc=`npal_disc=$disc`nbase=PAL`n"
    [IO.File]::WriteAllText((Join-Path $config 'install.ini'),$install,[Text.UTF8Encoding]::new($false))
    # A portable normal launch must not inherit diagnostic input or fixture flags.
    foreach ($variable in @(Get-ChildItem Env: | Where-Object Name -Like 'LM_*')) {
        Remove-Item -LiteralPath ('Env:' + $variable.Name)
    }
    $env:APPDATA = Join-Path $profile 'appdata'
    $env:LM_CARD_DIR = $cards
    $env:LM_CACHE_DIR = $cache
    $env:LM_VR = '1'
    $env:LM_VR_FIRST_PERSON = if ($Mode -eq 'FirstPerson') { '1' } else { '0' }
    $env:LM_VR_VIEW_MODE = $viewMode
    $env:LM_OPENXR_LOADER = $loader
    $stamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID
    Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'bin') -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $logs "$stamp-output.log") `
        -RedirectStandardError (Join-Path $logs "$stamp-errors.log") | Out-Null
} catch {
    if ($CheckOnly) { throw }
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Luigi Mansion VR') | Out-Null
    exit 1
}
