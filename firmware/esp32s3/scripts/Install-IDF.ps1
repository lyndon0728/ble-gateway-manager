param(
    [string]$SdkRoot = "$env:LOCALAPPDATA/GW32/esp-idf-v5.5.6",
    [string]$ToolsRoot = "$env:LOCALAPPDATA/GW32/espressif",
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$sdkFull = [IO.Path]::GetFullPath($SdkRoot)
$toolsFull = [IO.Path]::GetFullPath($ToolsRoot)
if (-not (Test-Path -LiteralPath "$sdkFull/tools/idf.py")) {
    if (Test-Path -LiteralPath $sdkFull) { throw "SDK path exists but is incomplete: $sdkFull. Inspect it before retrying." }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sdkFull) | Out-Null
    git clone --branch v5.5.6 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git $sdkFull
    if ($LASTEXITCODE -ne 0) { throw 'Official SDK clone failed; no toolchain has been installed.' }
}
$version = (git -C $sdkFull describe --tags --exact-match).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'v5.5.6') { throw "Expected exact SDK tag v5.5.6, found $version" }
$env:IDF_TOOLS_PATH = $toolsFull
& $Python "$sdkFull/tools/idf_tools.py" install --targets esp32s3
if ($LASTEXITCODE -ne 0) { throw 'SDK tools installation failed' }
& $Python "$sdkFull/tools/idf_tools.py" install-python-env
if ($LASTEXITCODE -ne 0) { throw 'SDK Python environment installation failed' }
Write-Output "Installed ESP-IDF v5.5.6 at $sdkFull; tools at $toolsFull"
Write-Output 'No device was flashed and no eFuse was modified.'
