param([Parameter(Mandatory=$true)][string]$Compiler,[Parameter(Mandatory=$true)][string]$CjsonDir)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$test=Join-Path $repo 'firmware/esp32s3/components/gw_actions/tests'
$output=Join-Path $repo '.tools/esp-gatt-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$args=@('-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',(Join-Path $test 'gatt_stubs'),'-I',$CjsonDir,(Join-Path $test 'test_gatt_nrf.c'),(Join-Path $CjsonDir 'cJSON.c'),'-o',(Join-Path $output 'test_gatt_nrf.exe'))
if([IO.Path]::GetFileName($Compiler) -match '^zig'){$args=@('cc')+$args}
& $Compiler @args
if($LASTEXITCODE -ne 0){throw 'GATT host compile failed'}
& (Join-Path $output 'test_gatt_nrf.exe')
if($LASTEXITCODE -ne 0){throw 'GATT host tests failed'}
