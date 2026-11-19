param(
    [string]$SdkRoot = "$env:LOCALAPPDATA/GW32/esp-idf-v5.5.6",
    [string]$ToolsRoot = "$env:LOCALAPPDATA/GW32/espressif",
    [string]$BuildDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkFull = [IO.Path]::GetFullPath($SdkRoot)
if (-not (Test-Path -LiteralPath "$sdkFull/export.ps1")) { throw 'Install the pinned SDK using Install-IDF.ps1 first.' }
$version = (git -C $sdkFull describe --tags --exact-match).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'v5.5.6') { throw "SDK must be v5.5.6, found $version" }
$env:IDF_TOOLS_PATH = [IO.Path]::GetFullPath($ToolsRoot)
. "$sdkFull/export.ps1"
Push-Location -LiteralPath $projectRoot
try {
    idf.py -B $BuildDirectory build
    if ($LASTEXITCODE -ne 0) { throw 'ESP32-S3 target build failed' }
    idf.py -B $BuildDirectory size
    if ($LASTEXITCODE -ne 0) { throw 'Image size check failed' }
} finally { Pop-Location }
