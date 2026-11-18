[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$NcsRoot,
    [ValidateSet('dk', 'gw32')][string]$Target = 'dk',
    [switch]$CommissionedRadio,
    [switch]$Dfu,
    [string]$SigningKey
)
$ErrorActionPreference = 'Stop'
$appRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$sdkRoot = (Resolve-Path -LiteralPath $NcsRoot).Path
$zephyrRoot = Join-Path $sdkRoot 'zephyr'
$manifestRoot = Join-Path $sdkRoot 'nrf'
if (!(Test-Path -LiteralPath (Join-Path $manifestRoot 'west.yml')) -or
    !(Test-Path -LiteralPath (Join-Path $zephyrRoot 'CMakeLists.txt'))) {
    throw 'NcsRoot must be a complete NCS workspace containing nrf/ and zephyr/.'
}
$west = Get-Command west -ErrorAction Stop
$git = Get-Command git -ErrorAction Stop
$sdkTag = & $git.Source -C $manifestRoot describe --tags --exact-match HEAD 2>$null
if ($LASTEXITCODE -ne 0 -or $sdkTag -ne 'v3.2.1') {
    throw 'This application baseline is NCS v3.2.1. Open its toolchain terminal and use its workspace.'
}
if ($CommissionedRadio -and $Target -ne 'gw32') {
    throw 'CommissionedRadio is only meaningful for the custom GW32 board.'
}
$board = if ($Target -eq 'dk') { 'nrf54l15dk/nrf54l15/cpuapp' } else { 'gw32_nrf54l15/nrf54l15/cpuapp' }
$suffix = if ($CommissionedRadio) { 'gw32-radio' } else { $Target }
if ($Dfu) { $suffix += '-dfu' }
$buildPath = Join-Path $appRoot "build-$suffix"
$cmakeArgs = @("-DBOARD_ROOT=$appRoot")
$extraConf = @()
if ($CommissionedRadio) {
    $extraConf += "$appRoot/boards/gw32-radio-commissioned.conf"
}
[string[]]$buildOptions = @()
if ($Dfu) {
    if (!$SigningKey) { throw 'DFU sysbuild requires an explicit board-specific ECDSA P-256 private signing key.' }
    $signingPath = (Resolve-Path -LiteralPath $SigningKey).Path
    if (!(Test-Path -LiteralPath $signingPath -PathType Leaf)) { throw 'SigningKey must name a key file.' }
    $exampleKey = Join-Path $sdkRoot 'bootloader/mcuboot/root-ec-p256.pem'
    if ((Test-Path -LiteralPath $exampleKey) -and
        (Get-FileHash -LiteralPath $exampleKey).Hash -eq (Get-FileHash -LiteralPath $signingPath).Hash) {
        throw 'The public MCUboot example private key is not an acceptable release signing key.'
    }
    $buildOptions = @('--sysbuild')
    $extraConf += "$appRoot/configs/dfu.conf"
    $cmakeArgs += "-DSB_CONF_FILE=$appRoot/sysbuild-dfu.conf"
    $cmakeArgs += ('-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="' + $signingPath.Replace('\','/') + '"')
}
if ($extraConf.Count) { $cmakeArgs += ('-DEXTRA_CONF_FILE=' + ($extraConf -join ';')) }
$savedZephyrBase = $env:ZEPHYR_BASE
try {
    $env:ZEPHYR_BASE = $zephyrRoot
    Push-Location -LiteralPath $appRoot
    try {
        & $west.Source build @buildOptions --pristine=always --board $board --build-dir $buildPath $appRoot -- @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "west build failed ($LASTEXITCODE)." }
    } finally { Pop-Location }
    $imageRoot = if ($Dfu) { Join-Path $buildPath 'nrf54l15' } else { $buildPath }
    $config = Get-Content -LiteralPath (Join-Path $imageRoot 'zephyr/.config') -Raw
    foreach ($required in @('CONFIG_CSPRNG_ENABLED=y', 'CONFIG_CLOCK_CONTROL_MPSL=y', 'CONFIG_UART_20_ASYNC=y')) {
        if (!$config.Contains($required)) { throw "Generated configuration missing $required" }
    }
    if ($Dfu -and !$config.Contains('CONFIG_GW_DFU=y')) { throw 'DFU was not enabled in the generated application configuration.' }
    Write-Output "Built $board at $imageRoot/zephyr/zephyr.hex"
    if ($Dfu) {
        Write-Output "UART upgrade artifact: $imageRoot/zephyr/zephyr.signed.bin"
        Write-Output 'First installation needs the matching MCUboot bootloader and partition layout via SWD.'
    }
    Write-Output 'No device was flashed.'
} finally { $env:ZEPHYR_BASE = $savedZephyrBase }
