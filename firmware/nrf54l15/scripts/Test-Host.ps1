[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$appRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$buildPath = Join-Path $appRoot 'build-host'
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
$prefix = @()
if ((Split-Path $compilerPath -Leaf) -eq 'zig.exe') { $prefix = @('cc') }
$flags = @('-std=c11', '-Wall', '-Wextra', '-Werror')
Push-Location -LiteralPath $appRoot
try {
    $coreExe = Join-Path $buildPath 'test_core.exe'
    & $compilerPath @prefix @flags '-Isrc' 'src/gw_core.c' 'tests/test_core.c' '-o' $coreExe
    if ($LASTEXITCODE -ne 0) { throw 'Protocol core compilation failed.' }
    & $coreExe
    if ($LASTEXITCODE -ne 0) { throw 'Protocol core tests failed.' }
    $filterExe = Join-Path $buildPath 'test_filter_v2.exe'
    & $compilerPath @prefix @flags '-Isrc' 'src/gw_core.c' 'src/gw_filter_v2.c' 'tests/test_filter_v2.c' '-o' $filterExe
    if ($LASTEXITCODE -ne 0) { throw 'Beacon V2 compilation failed.' }
    & $filterExe
    if ($LASTEXITCODE -ne 0) { throw 'Beacon V2 tests failed.' }
    $gattExe = Join-Path $buildPath 'test_gatt.exe'
    & $compilerPath @prefix @flags '-Itests/gatt_mock' 'tests/test_gatt.c' '-o' $gattExe
    if ($LASTEXITCODE -ne 0) { throw 'GATT lifecycle test compilation failed.' }
    & $gattExe
    if ($LASTEXITCODE -ne 0) { throw 'GATT lifecycle tests failed.' }
    $radioExe = Join-Path $buildPath 'test_radio.exe'
    & $compilerPath @prefix @flags '-Itests/radio_mock' '-Isrc' 'src/gw_radio.c' 'src/gw_core.c' 'src/gw_filter_v2.c' 'tests/test_radio.c' '-o' $radioExe
    if ($LASTEXITCODE -ne 0) { throw 'Radio behavior test compilation failed.' }
    & $radioExe
    if ($LASTEXITCODE -ne 0) { throw 'Radio behavior tests failed.' }
    $appExe = Join-Path $buildPath 'test_app.exe'
    & $compilerPath @prefix @flags '-Itests/app_mock' 'tests/test_app.c' 'src/gw_core.c' '-o' $appExe
    if ($LASTEXITCODE -ne 0) { throw 'UART application test compilation failed.' }
    & $appExe
    if ($LASTEXITCODE -ne 0) { throw 'UART application tests failed.' }
    $peripheralExe = Join-Path $buildPath 'test_peripheral.exe'
    & $compilerPath @prefix @flags '-Itests/peripheral_mock' 'tests/test_peripheral.c' '-o' $peripheralExe
    if ($LASTEXITCODE -ne 0) { throw 'Peripheral lifecycle compilation failed.' }
    & $peripheralExe
    if ($LASTEXITCODE -ne 0) { throw 'Peripheral lifecycle tests failed.' }
    $dfuExe = Join-Path $buildPath 'test_dfu.exe'
    & $compilerPath @prefix @flags '-Itests/dfu_mock' 'tests/test_dfu.c' 'src/gw_core.c' '-o' $dfuExe
    if ($LASTEXITCODE -ne 0) { throw 'UART DFU compilation failed.' }
    & $dfuExe
    if ($LASTEXITCODE -ne 0) { throw 'UART DFU tests failed.' }
} finally { Pop-Location }
