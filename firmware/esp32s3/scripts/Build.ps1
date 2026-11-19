param(
    [string]$SdkRoot = "$env:LOCALAPPDATA/GW32/esp-idf-v5.5.6",
    [string]$ToolsRoot = "$env:LOCALAPPDATA/GW32/espressif",
    [string]$BuildDirectory = 'build',
    [string]$AsciiBuildRoot = "$env:LOCALAPPDATA/GW32/builds",
    [switch]$LocalSetupTest
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkFull = [IO.Path]::GetFullPath($SdkRoot)
$toolsFull = [IO.Path]::GetFullPath($ToolsRoot)
if ($LocalSetupTest -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build-local-setup-test'
}
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
    $idfArguments = @('-B', $BuildDirectory)
    if ($LocalSetupTest) {
        # SDKCONFIG_DEFAULTS does not override a cached sdkconfig. Keep this
        # evaluation profile isolated from every board/production configuration.
        $testBuildFull = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
            [IO.Path]::GetFullPath($BuildDirectory)
        } else {
            [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
        }
        if (Test-Path -LiteralPath $testBuildFull) {
            throw 'LocalSetupTest requires a new BuildDirectory. Choose a fresh directory; no existing build/configuration will be overwritten.'
        }
        New-Item -ItemType Directory -Path $testBuildFull | Out-Null
        $testSdkconfig = Join-Path $testBuildFull 'sdkconfig.local-setup-test'
        $testDefaults = (Join-Path $projectRoot 'sdkconfig.defaults') + ';' + (Join-Path $projectRoot 'sdkconfig.local-setup-test')
        $idfArguments += @('-D', "SDKCONFIG=$testSdkconfig", '-D', "SDKCONFIG_DEFAULTS=$testDefaults")
        Write-Output 'Local setup evaluation profile: first unconfigured boot may open one 600-second setup window. A verified production authorization/button design is still required.'
    }
    & idf.py @idfArguments build
    if ($LASTEXITCODE -ne 0) { throw 'ESP32-S3 target build failed' }
    idf.py -B $BuildDirectory size
    if ($LASTEXITCODE -ne 0) { throw 'Image size check failed' }
} finally { Pop-Location }
