param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$serverRoot=Join-Path $repoRoot 'server';$runtime=Join-Path $serverRoot '.local/frontend-e2e';$stateFile=Join-Path $runtime 'fixture.json'
$state=Get-Content -LiteralPath $stateFile -Raw|ConvertFrom-Json
& "$PSScriptRoot/fixture.ps1" -Action Stop
if(-not $SkipBuild){& "$serverRoot/scripts/build.ps1"}
$children=[Collections.Generic.List[object]]::new()
function Child([string]$Name,[string[]]$Arguments,[string]$Tag){$path=Join-Path $serverRoot "bin/$Name.exe";$p=Start-Process -FilePath $path -ArgumentList $Arguments -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runtime "$Tag.out.log") -RedirectStandardError (Join-Path $runtime "$Tag.err.log");$children.Add(@{pid=$p.Id;path=$path;startedAt=$p.StartTime.ToUniversalTime().ToString('o')});return $p}
function Wait-HTTP([string]$Url){for($i=0;$i -lt 100;$i++){foreach($child in $children){if(-not(Get-Process -Id $child.pid -ErrorAction SilentlyContinue)){throw 'Fixture process exited early; inspect runtime logs.'}};try{$null=Invoke-RestMethod $Url -TimeoutSec 1;return}catch{Start-Sleep -Milliseconds 100}};throw "Endpoint not ready: $Url"}
$previousHosts=$env:GW_ALLOWED_TARGET_HOSTS
try{
    $env:GW_ALLOWED_TARGET_HOSTS='127.0.0.1'
    $null=Child gateway-server @('--data-dir','data','--listen',"127.0.0.1:$(([Uri]$state.baseUrl).Port)",'--allow-insecure-dev') server
    Wait-HTTP "$($state.baseUrl)/health/ready"
    $null=Child customer-simulator @('--listen',"127.0.0.1:$(([Uri]$state.customerUrl).Port)",'--db','customer.db','--target',$state.targetId,'--target-version','1') customer
    Wait-HTTP "$($state.customerUrl)/health/live"
    $null=Child gateway-simulator @('--server',$state.baseUrl,'--gateway','gw32_ui','--profile','gw32-profile-1','--db','gw32.db','--token-file','secrets/gw32-token','--count','0','--duration','0','--interval','200ms') gw32
    $null=Child gateway-simulator @('--server',$state.baseUrl,'--gateway','gw_reference_ui','--db','reference.db','--token-file','secrets/reference-token','--count','0','--duration','0','--interval','200ms') reference
    $state.children=@($children.ToArray());$state|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $stateFile -Encoding utf8
    Write-Host "Restarted isolated fixture: $($state.baseUrl)/ui/"
}catch{foreach($child in $children){Stop-Process -Id $child.pid -ErrorAction SilentlyContinue};throw}finally{$env:GW_ALLOWED_TARGET_HOSTS=$previousHosts}
