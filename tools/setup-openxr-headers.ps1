$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = Join-Path $projectRoot '.tools\openxr'
New-Item -ItemType Directory -Force -Path (Join-Path $sdkRoot 'include\openxr') | Out-Null
$sdkUrl = 'https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/release-1.1.54'
foreach ($name in @('openxr.h', 'openxr_platform.h', 'openxr_platform_defines.h')) {
    Invoke-WebRequest -Uri "$sdkUrl/include/openxr/$name" -OutFile (Join-Path $sdkRoot "include\openxr\$name")
}
Invoke-WebRequest -Uri "$sdkUrl/LICENSE" -OutFile (Join-Path $sdkRoot 'LICENSE')
