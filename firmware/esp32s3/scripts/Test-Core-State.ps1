param([Parameter(Mandatory=$true)][string]$Compiler,[Parameter(Mandatory=$true)][string]$CjsonDir)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$core=Join-Path $repo 'firmware/esp32s3/components/gw_core'
$stubs=Join-Path $repo 'firmware/esp32s3/components/gw_actions/tests/gatt_stubs'
$output=Join-Path $repo '.tools/esp-core-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$args=@('-std=c11','-Wall','-Wextra','-Werror','-I',(Join-Path $core 'include'),'-I',(Join-Path $core 'tests/state_stubs'),'-I',$stubs,'-I',$CjsonDir,(Join-Path $core 'tests/test_state.c'),(Join-Path $core 'gw_rules.c'),(Join-Path $CjsonDir 'cJSON.c'),'-o',(Join-Path $output 'test_state.exe'))
if([IO.Path]::GetFileName($Compiler) -match '^zig'){$args=@('cc')+$args}
& $Compiler @args
if($LASTEXITCODE -ne 0){throw 'Core state host compile failed'}
& (Join-Path $output 'test_state.exe')
if($LASTEXITCODE -ne 0){throw 'Core state host tests failed'}
