$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'docs\base-game-manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest) {
    $file = Join-Path (Join-Path $projectRoot 'base-game') $entry.path
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing baseline file: $($entry.path)" }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Baseline file changed: $($entry.path)"
    }
}
Write-Host "Verified $($manifest.Count) frozen baseline files."
