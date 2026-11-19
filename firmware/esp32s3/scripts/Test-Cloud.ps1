param([string]$Compiler='cc',[Parameter(Mandatory=$true)][string]$CJsonDirectory)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot '../..'))
$output=Join-Path $repoRoot '.tools/esp-cloud-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$includes=@('gw_cloud/tests/mocks','gw_cloud/include','gw_core/include','gw_http/include','gw_nrf/include','gw_wire/include','gw_actions/include')
$argsList=@('-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror')
foreach($inc in $includes){$argsList+=@('-I',"$projectRoot/components/$inc")}
$argsList+=@('-I',$CJsonDirectory,"$projectRoot/components/gw_cloud/tests/test_cloud.c",(Join-Path $CJsonDirectory 'cJSON.c'),'-o',"$output/cloud.exe")
if([IO.Path]::GetFileNameWithoutExtension($Compiler)-eq 'zig'){& $Compiler cc @argsList}else{& $Compiler @argsList}
if($LASTEXITCODE-ne 0){throw 'Cloud test build failed'}
& "$output/cloud.exe"
if($LASTEXITCODE-ne 0){throw 'Cloud tests failed'}
