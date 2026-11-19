param(
    [string]$SdkRoot = "$env:LOCALAPPDATA/GW32/esp-idf-v5.5.6",
    [string]$ToolsRoot = "$env:LOCALAPPDATA/GW32/espressif",
    [string]$BuildDirectory = 'build',
    [string]$AsciiBuildRoot = "$env:LOCALAPPDATA/GW32/builds"
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkFull = [IO.Path]::GetFullPath($SdkRoot)
$toolsFull = [IO.Path]::GetFullPath($ToolsRoot)
if ($sdkFull -match '[^\x00-\x7F]' -or $toolsFull -match '[^\x00-\x7F]') {
    throw 'The SDK and toolchain paths must contain ASCII characters only on Windows.'
}
if (-not (Test-Path -LiteralPath "$sdkFull/export.ps1")) { throw 'Install the pinned SDK using Install-IDF.ps1 first.' }
$version = (git -C $sdkFull describe --tags --exact-match).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'v5.5.6') { throw "SDK must be v5.5.6, found $version" }
$env:IDF_TOOLS_PATH = $toolsFull
. "$sdkFull/export.ps1"
if ($projectRoot -match '[^\x00-\x7F]') {
    # idf.py resolves junctions to their original path, so use a fresh build-only
    # copy. No /MIR or deletion: never remove user source or an earlier build.
    $stageRoot = [IO.Path]::GetFullPath($AsciiBuildRoot)
    if ($stageRoot -match '[^\x00-\x7F]') { throw 'AsciiBuildRoot must be an ASCII path.' }
    $stage = Join-Path $stageRoot ([Guid]::NewGuid().ToString('N'))
    $stageProject = Join-Path $stage 'firmware/esp32s3'
    $sharedSource = [IO.Path]::GetFullPath((Join-Path $projectRoot '../nrf54l15/src'))
    New-Item -ItemType Directory -Force -Path $stageProject | Out-Null
    robocopy $projectRoot $stageProject /E /XJ /XD build managed_components /NFL /NDL /NJH /NJS /NC /NS | Out-Null
    if ($LASTEXITCODE -gt 7) { throw 'Failed to prepare ASCII firmware source copy' }
    robocopy $sharedSource (Join-Path $stage 'firmware/nrf54l15/src') /E /XJ /NFL /NDL /NJH /NJS /NC /NS | Out-Null
    if ($LASTEXITCODE -gt 7) { throw 'Failed to prepare shared UART codec source copy' }
    $projectRoot = $stageProject
    $BuildDirectory = Join-Path $stage 'build'
    Write-Output "Building verified source copy at $stage; outputs are kept there."
}
Push-Location -LiteralPath $projectRoot
try {
    idf.py -B $BuildDirectory build
    if ($LASTEXITCODE -ne 0) { throw 'ESP32-S3 target build failed' }
    idf.py -B $BuildDirectory size
    if ($LASTEXITCODE -ne 0) { throw 'Image size check failed' }
} finally { Pop-Location }
