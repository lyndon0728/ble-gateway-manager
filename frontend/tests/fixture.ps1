param([ValidateSet('Start','Stop','Restart')][string]$Action='Start',[int]$Port=18781,[int]$CustomerPort=19191,[switch]$SkipBuild,[ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Instance='frontend-e2e')
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$serverRoot=Join-Path $repoRoot 'server'
$runtime=Join-Path $serverRoot ".local/$Instance"
$stateFile=Join-Path $runtime 'fixture.json'
if($Action -eq 'Restart'){& "$PSScriptRoot/restart-fixture.ps1" -SkipBuild:$SkipBuild -Instance $Instance;return}
if($Action -eq 'Stop'){
    if(Test-Path -LiteralPath $stateFile){$state=Get-Content -LiteralPath $stateFile -Raw|ConvertFrom-Json;foreach($child in $state.children){$proc=Get-Process -Id $child.pid -ErrorAction SilentlyContinue;if($proc){if([IO.Path]::GetFullPath($proc.Path) -ne [IO.Path]::GetFullPath($child.path) -or $proc.StartTime.ToUniversalTime().Ticks -ne ([DateTime]$child.startedAt).ToUniversalTime().Ticks){throw 'Fixture PID identity changed; refusing to stop a different process.'};Stop-Process -Id $proc.Id;$proc.WaitForExit()}}}
    Write-Host 'Stopped frontend integration fixture; evidence retained.';return
}
if(Test-Path -LiteralPath (Join-Path $runtime 'data/management.db')){throw 'Fixture data already exists. Reuse the running fixture or choose a new isolated fixture directory in a separate checkout; data is never deleted automatically.'}
foreach($p in @($Port,$CustomerPort)){$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$p);try{$probe.Start()}finally{$probe.Stop()}}
if(-not $SkipBuild){& "$serverRoot/scripts/build.ps1"}
New-Item -ItemType Directory -Force -Path $runtime|Out-Null
$children=[Collections.Generic.List[object]]::new();$base="http://127.0.0.1:$Port";$customer="http://127.0.0.1:$CustomerPort"
function Child([string]$Name,[string[]]$Arguments,[string]$Tag){$path=Join-Path $serverRoot "bin/$Name.exe";$p=Start-Process -FilePath $path -ArgumentList $Arguments -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runtime "$Tag.out.log") -RedirectStandardError (Join-Path $runtime "$Tag.err.log");$children.Add(@{pid=$p.Id;path=$path;startedAt=$p.StartTime.ToUniversalTime().ToString('o')});return $p}
function Wait-HTTP([string]$Url){for($i=0;$i -lt 100;$i++){try{$null=Invoke-RestMethod $Url -TimeoutSec 1;return}catch{Start-Sleep -Milliseconds 100}};throw "Endpoint not ready: $Url"}
function API([string]$Method,[string]$Path,$Body=$null,[hashtable]$Extra=@{}){$headers=@{'X-CSRF-Token'=$script:csrf;'Idempotency-Key'=[Guid]::NewGuid().ToString('N')};foreach($key in $Extra.Keys){$headers[$key]=$Extra[$key]};$params=@{Method=$Method;Uri="$base$Path";WebSession=$script:session;Headers=$headers;TimeoutSec=10};if($null -ne $Body){$params.ContentType='application/json';$params.Body=ConvertTo-Json -InputObject $Body -Depth 40 -Compress};Invoke-RestMethod @params}
$previousHosts=$env:GW_ALLOWED_TARGET_HOSTS
Push-Location $runtime
try{
    & "$serverRoot/bin/gatewayctl.exe" init --data-dir data --password-file secrets/admin-password
    if($LASTEXITCODE -ne 0){throw 'init failed'}
    & "$serverRoot/bin/gatewayctl.exe" provision-gateway --data-dir data --gateway gw32_ui --token-file secrets/gw32-token --profile gw32-profile-1
    if($LASTEXITCODE -ne 0){throw 'GW32 provision failed'}
    & "$serverRoot/bin/gatewayctl.exe" provision-gateway --data-dir data --gateway gw_reference_ui --token-file secrets/reference-token
    if($LASTEXITCODE -ne 0){throw 'reference provision failed'}
    $env:GW_ALLOWED_TARGET_HOSTS='127.0.0.1'
    $null=Child gateway-server @('--data-dir','data','--listen',"127.0.0.1:$Port",'--allow-insecure-dev') server
    Wait-HTTP "$base/health/ready"
    $script:session=[Microsoft.PowerShell.Commands.WebRequestSession]::new()
    $login=Invoke-RestMethod -Method Post -Uri "$base/api/v1/auth/sessions" -WebSession $script:session -ContentType application/json -Body (@{name='admin';password=(Get-Content -LiteralPath secrets/admin-password -Raw).Trim()}|ConvertTo-Json -Compress)
    $script:csrf=$login.csrfToken
    $target=API POST '/api/v1/targets' @{name='UI TEST durable receiver';configuration=@{protocol='HTTP';endpoint="$customer/events";ackMode='HTTP_DURABLE_RESPONSE';maxBatchEvents=200;maxInflightEvents=200;maxQueueBytes=10485760;mappingVersion='identity-1'}}
    $retryTarget=API POST '/api/v1/targets' @{name='UI TEST disconnected receiver';configuration=@{protocol='HTTP';endpoint='http://127.0.0.1:19192/events';ackMode='HTTP_DURABLE_RESPONSE';maxBatchEvents=200;maxInflightEvents=200;maxQueueBytes=10485760;mappingVersion='identity-1'}}
    $null=Child customer-simulator @('--listen',"127.0.0.1:$CustomerPort",'--db','customer.db','--target',$target.targetId,'--target-version','1','--drop-acks','1') customer
    Wait-HTTP "$customer/health/live"
    $refs=@(@{targetId=$target.targetId;version='1';mappingVersion='identity-1';required=$true},@{targetId=$retryTarget.targetId;version='1';mappingVersion='identity-1';required=$false})
    $rules=@(@{ruleId='ui_reference';priority=100;match=@{modelIds=@('REFERENCE_GW');protocolVersions=@('reference-sensor-v1')};action='DECODE';parserId='reference_sensor';parserVersion='1.0.0';schemaVersion='reference-sensor-1';targetVersions=$refs},@{ruleId='ui_ble_ad';priority=100;match=@{modelIds=@('GW32I33V2');protocolVersions=@('ble-ad-v1')};action='DECODE';parserId='ble_ad';parserVersion='1.0.0';schemaVersion='ble-ad-1';targetVersions=$refs},@{ruleId='ui_gatt';priority=90;match=@{modelIds=@('GW32I33V2');protocolVersions=@('ble-gatt-value-v1')};action='PASSTHROUGH';targetVersions=$refs})
    $bundle=API POST '/api/v1/rule-bundles' @{name='UI TEST seeded pipeline';defaultAction='QUARANTINE';rules=$rules}
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/validations" @{}
    $status=API GET '/api/v1/system/status';$node=$status.dataNodes[0]
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/activations" @{dataNodeId=$node.dataNodeId;expectedActivationRevision=$node.activationRevision} @{'If-Match'=$node.activationEtag}
    $null=Child gateway-simulator @('--server',$base,'--gateway','gw32_ui','--profile','gw32-profile-1','--db','gw32.db','--token-file','secrets/gw32-token','--count','4','--duration','0','--interval','200ms','--ignore-acks','1','--ignore-result-acks','1') gw32
    $null=Child gateway-simulator @('--server',$base,'--gateway','gw_reference_ui','--db','reference.db','--token-file','secrets/reference-token','--count','4','--duration','0','--interval','200ms','--ignore-acks','1') reference
    @{baseUrl=$base;passwordFile=(Join-Path $runtime 'secrets/admin-password');gatewayId='gw32_ui';referenceGatewayId='gw_reference_ui';targetId=$target.targetId;retryTargetId=$retryTarget.targetId;bundleId=$bundle.bundleId;customerUrl=$customer;children=@($children.ToArray())}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $stateFile -Encoding utf8
    Write-Host "Frontend integration fixture ready: $base/ui/"
    Write-Host "Non-secret metadata: $stateFile"
}catch{foreach($child in $children){Stop-Process -Id $child.pid -ErrorAction SilentlyContinue};throw}finally{$env:GW_ALLOWED_TARGET_HOSTS=$previousHosts;Pop-Location}
