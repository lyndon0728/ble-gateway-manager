param([string]$Compiler = 'cc')
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot '../..'))
$output=Join-Path $repoRoot '.tools/esp-core-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sources=@("$projectRoot/components/gw_core/tests/test_rules.c","$projectRoot/components/gw_core/gw_rules.c")
$argsList=@('-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I',"$projectRoot/components/gw_core")+$sources+@('-o',"$output/core-rules.exe")
if ([IO.Path]::GetFileNameWithoutExtension($Compiler) -eq 'zig') { & $Compiler cc @argsList } else { & $Compiler @argsList }
if ($LASTEXITCODE -ne 0) { throw 'Core host test compilation failed' }
& "$output/core-rules.exe"
if ($LASTEXITCODE -ne 0) { throw 'Core host tests failed' }
