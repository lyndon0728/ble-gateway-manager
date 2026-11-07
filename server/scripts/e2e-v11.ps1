param([switch]$SkipBuild,[int]$ServerPort=0,[int]$CustomerPort=0)
$ErrorActionPreference='Stop'
$serverRoot=Split-Path -Parent $PSScriptRoot
if(-not $SkipBuild){& "$PSScriptRoot/build.ps1"}
function Free-Port { $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);try{$probe.Start();return $probe.LocalEndpoint.Port}finally{$probe.Stop()} }
if($ServerPort -eq 0){$ServerPort=Free-Port};if($CustomerPort -eq 0){$CustomerPort=Free-Port}
foreach($port in @($ServerPort,$CustomerPort)){$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$port);try{$probe.Start()}catch{throw "Port $port is occupied; choose another port."}finally{$probe.Stop()}}
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('ble-gateway-v11-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
$binary=Join-Path $serverRoot 'bin';$base="http://127.0.0.1:$ServerPort";$customer="http://127.0.0.1:$CustomerPort"
$children=[Collections.Generic.List[Diagnostics.Process]]::new();$oldHosts=$env:GW_ALLOWED_TARGET_HOSTS
function Start-Child([string]$Name,[string[]]$Arguments,[string]$Tag){$process=Start-Process -FilePath (Join-Path $binary "$Name.exe") -ArgumentList $Arguments -WorkingDirectory $testRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $testRoot "$Tag.out.log") -RedirectStandardError (Join-Path $testRoot "$Tag.err.log");$children.Add($process);return $process}
function Wait-HTTP([string]$Url){for($i=0;$i -lt 100;$i++){foreach($child in $children){if($child.HasExited){throw "Process exited early; see $testRoot"}};try{$null=Invoke-RestMethod $Url -TimeoutSec 1;return}catch{Start-Sleep -Milliseconds 100}};throw "Endpoint not ready: $Url"}
function API([string]$Method,[string]$Path,$Body=$null,[hashtable]$Extra=@{}){$headers=@{'X-CSRF-Token'=$script:csrf;'Idempotency-Key'=[Guid]::NewGuid().ToString('N')};foreach($key in $Extra.Keys){$headers[$key]=$Extra[$key]};$request=@{Method=$Method;Uri="$base$Path";WebSession=$script:session;Headers=$headers;TimeoutSec=10};if($null -ne $Body){$request.ContentType='application/json';$request.Body=ConvertTo-Json -InputObject $Body -Depth 30 -Compress};return Invoke-RestMethod @request}
function New-Task([string]$Kind,[hashtable]$Parameters){return API POST '/api/v1/tasks' @{gatewayId='gw32_sim';type=$Kind;parameters=$Parameters;expiresAt=[DateTime]::UtcNow.AddMinutes(2).ToString('o')}}
function Wait-Task([string]$ID){$deadline=[DateTime]::UtcNow.AddSeconds(20);do{$task=API GET "/api/v1/tasks/$ID";if($task.state -eq 'SUCCEEDED'){if($task.result.verification -ne 'VERIFIED'){throw 'Unverified task success'};return $task};if($task.state -in @('FAILED','EXPIRED','CANCELLED','UNKNOWN')){throw "Task did not succeed: $($task|ConvertTo-Json -Depth 15 -Compress)"};Start-Sleep -Milliseconds 200}while([DateTime]::UtcNow -lt $deadline);throw "Task timeout: $ID"}
Push-Location $testRoot
try{
    & "$binary/gatewayctl.exe" init --data-dir data --password-file secrets/admin-password
    if($LASTEXITCODE -ne 0){throw 'init failed'}
    & "$binary/gatewayctl.exe" provision-gateway --data-dir data --gateway gw_legacy --token-file secrets/legacy-token
    if($LASTEXITCODE -ne 0){throw 'legacy provision failed'}
    & "$binary/gatewayctl.exe" provision-gateway --data-dir data --gateway gw32_sim --token-file secrets/gw32-token --profile gw32-profile-1
    if($LASTEXITCODE -ne 0){throw 'GW32 simulation provision failed'}
    $env:GW_ALLOWED_TARGET_HOSTS='127.0.0.1'
    $server=Start-Child gateway-server @('--data-dir','data','--listen',"127.0.0.1:$ServerPort",'--allow-insecure-dev') server
    Wait-HTTP "$base/health/ready"
    $script:session=[Microsoft.PowerShell.Commands.WebRequestSession]::new()
    $login=Invoke-RestMethod -Method Post -Uri "$base/api/v1/auth/sessions" -WebSession $script:session -ContentType application/json -Body (@{name='admin';password=(Get-Content -LiteralPath secrets/admin-password -Raw).Trim()}|ConvertTo-Json -Compress)
    $script:csrf=$login.csrfToken
    $target=API POST '/api/v1/targets' @{name='protocol 1.1 durable customer';configuration=@{protocol='HTTP';endpoint="$customer/events";ackMode='HTTP_DURABLE_RESPONSE';maxBatchEvents=200;maxInflightEvents=200;maxQueueBytes=10485760;mappingVersion='identity-1'}}
    $receiver=Start-Child customer-simulator @('--listen',"127.0.0.1:$CustomerPort",'--db','customer.db','--target',$target.targetId,'--target-version','1','--drop-acks','1') customer
    Wait-HTTP "$customer/health/live"
    $targetRef=@{targetId=$target.targetId;version='1';mappingVersion='identity-1';required=$true}
    $rules=@(
      @{ruleId='legacy_reference';priority=100;match=@{modelIds=@('REFERENCE_GW');protocolVersions=@('reference-sensor-v1')};action='DECODE';parserId='reference_sensor';parserVersion='1.0.0';schemaVersion='reference-sensor-1';targetVersions=@($targetRef)},
      @{ruleId='gw32_ad';priority=100;match=@{modelIds=@('GW32I33V2');protocolVersions=@('ble-ad-v1')};action='DECODE';parserId='ble_ad';parserVersion='1.0.0';schemaVersion='ble-ad-1';targetVersions=@($targetRef)},
      @{ruleId='gw32_gatt';priority=90;match=@{modelIds=@('GW32I33V2');protocolVersions=@('ble-gatt-value-v1')};action='PASSTHROUGH';targetVersions=@($targetRef)}
    )
    $bundle=API POST '/api/v1/rule-bundles' @{name='legacy and GW32 coexistence';defaultAction='QUARANTINE';rules=$rules}
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/validations" @{}
    $status=API GET '/api/v1/system/status';$node=$status.dataNodes[0]
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/activations" @{dataNodeId=$node.dataNodeId;expectedActivationRevision=$node.activationRevision} @{'If-Match'=$node.activationEtag}
    $gatewayInfo=API GET '/api/v1/gateways/gw32_sim'
    $peer=API POST '/api/v1/ble-peers' @{scopeId=$gatewayInfo.groupId;displayName='simulated registered peer';deviceAddress='AABBCCDDEEFF';addressType='PUBLIC'}
    $service='12345678-1234-5678-9abc-def012345678';$characteristic='12345678-1234-5678-9abc-def012345679'
    $autoRule=API POST '/api/v1/auto-connect-rules' @{name='UUID in scan response';peerId=$peer.peerId;gatewayIds=@('gw32_sim');enabled=$true;priority=100;serviceUuid=$service;characteristicUuid=$characteristic;wireBits=128;security='UNENCRYPTED';maxAgeMs=5000;cooldownMs=30000}
    $legacy=Start-Child gateway-simulator @('--server',$base,'--gateway','gw_legacy','--db','legacy.db','--token-file','secrets/legacy-token','--count','2','--duration','60s','--interval','200ms','--ignore-acks','1') legacy
    $gw32=Start-Child gateway-simulator @('--server',$base,'--gateway','gw32_sim','--profile','gw32-profile-1','--db','gw32.db','--token-file','secrets/gw32-token','--count','1','--duration','60s','--interval','200ms','--ignore-acks','1','--ignore-result-acks','1') gw32
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do{Start-Sleep -Milliseconds 100;$tasks=API GET '/api/v1/tasks?gatewayId=gw32_sim';$connectTasks=@($tasks.items|Where-Object {$_.type -eq 'GATT_CONNECT'})}while($connectTasks.Count -eq 0 -and [DateTime]::UtcNow -lt $deadline)
    if($connectTasks.Count -ne 1){throw "Expected one automatically created CONNECT task, got $($connectTasks.Count)"}
    $connected=Wait-Task $connectTasks[0].taskId;$connectionRef=$connected.result.details.connectionRef
    if($connectionRef -notmatch '^[0-9a-f]{32}$'){throw 'Invalid connectionRef'}
    if($connected.result.details.ruleId -ne $autoRule.ruleId -or $connected.result.details.serviceUuid -ne $service){throw 'Auto-connect discovery evidence does not match the rule'}
    $null=API PATCH "/api/v1/auto-connect-rules/$($autoRule.ruleId)" @{enabled=$false} @{'If-Match'='"1"'}
    $subscribe=New-Task GATT_SUBSCRIBE @{radio='NRF54';connectionRef=$connectionRef;serviceUuid=$service;characteristicUuid=$characteristic;mode='NOTIFY';timeoutMs=5000}
    $subscribed=Wait-Task $subscribe.taskId
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    do{Start-Sleep -Milliseconds 200;$stats=Invoke-RestMethod "$customer/stats"}while(($stats.businessRecords -lt 5 -or $stats.requests -lt 2)-and[DateTime]::UtcNow -lt $deadline)
    if($stats.businessRecords -ne 5 -or $stats.deliveries -ne 5){throw "Expected two legacy, ADV+SCAN_RSP and one notify; got $($stats|ConvertTo-Json -Compress)"}
    $unsubscribe=New-Task GATT_UNSUBSCRIBE @{radio='NRF54';connectionRef=$connectionRef;serviceUuid=$service;characteristicUuid=$characteristic;timeoutMs=5000}
    $null=Wait-Task $unsubscribe.taskId
    $disconnect=New-Task GATT_DISCONNECT @{radio='NRF54';connectionRef=$connectionRef;timeoutMs=5000}
    $disconnected=Wait-Task $disconnect.taskId
    $network=New-Task CONFIGURE @{expectedReportedRevision=0;configuration=@{network=@{uplinkOrder=@('ETHERNET','WIFI_STA');ethernet=@{enabled=$true;ipv4=@{mode='DHCP'}};wifiSta=@{enabled=$true;ssid='SIMULATED-NETWORK';security='WPA2_PSK';credentialRef='simulated-local-credential';ipv4=@{mode='DHCP'}}}}}
    $networkApplied=Wait-Task $network.taskId
    $from=[Uri]::EscapeDataString([DateTime]::UtcNow.AddMinutes(-5).ToString('o'));$to=[Uri]::EscapeDataString([DateTime]::UtcNow.AddMinutes(1).ToString('o'))
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do{$events=API GET "/api/v1/events?gatewayId=gw32_sim&from=$from&to=$to&limit=200";if(@($events.items).Count -ge 4){break};Start-Sleep -Milliseconds 200}while([DateTime]::UtcNow -lt $deadline)
    $rawEvents=@();foreach($event in $events.items){$view=API GET "/api/v1/gateways/gw32_sim/events/$($event.eventId)?includeRaw=true";$rawEvents+=,$view.raw}
    $adv=@($rawEvents|Where-Object {$_.capture.kind -eq 'ADV'});$scan=@($rawEvents|Where-Object {$_.capture.kind -eq 'SCAN_RSP'});$notify=@($rawEvents|Where-Object {$_.capture.kind -eq 'GATT_NOTIFY'});$down=@($rawEvents|Where-Object {$_.capture.kind -eq 'GATT_LINK_DOWN'})
    if($adv.Count -ne 1 -or @($adv[0].capture.observedBy).Count -ne 2 -or $scan.Count -ne 1 -or $scan[0].capture.radio -ne 'ESP32' -or $notify.Count -ne 1 -or $down.Count -ne 1){throw 'Durable capture/link-down evidence is incomplete'}
    $connections=API GET '/api/v1/gatt-connections?gatewayId=gw32_sim';$closed=@($connections.items|Where-Object {$_.connectionRef -eq $connectionRef -and $_.state -eq 'CLOSED'})
    if($closed.Count -ne 1){throw 'Connection was not closed on the server'}
    $allTasks=API GET '/api/v1/tasks?gatewayId=gw32_sim';$autoCount=@($allTasks.items|Where-Object {$_.type -eq 'GATT_CONNECT'}).Count
    if($autoCount -ne 1){throw 'Duplicate ingress created duplicate automatic CONNECT tasks'}
    $report=@{passed=$true;at=[DateTime]::UtcNow.ToString('o');transport='HTTP loopback';legacyEvents=2;captureEvents=2;notificationEvents=1;linkDownEvents=$down.Count;customerBusinessRecords=$stats.businessRecords;customerRequests=$stats.requests;connectionRef=$connectionRef;automaticConnectTaskCount=$autoCount;uuidSource='ESP32 SCAN_RSP';connect=$connected.state;subscribe=$subscribed.state;disconnect=$disconnected.state;connectionClosed=$true;network=$networkApplied.state;networkReportedRevision=$networkApplied.result.details.reportedConfigurationRevision;serverGW32EventCount=@($events.items).Count;hardwareValidated=$false;evidenceDirectory=$testRoot}
    $report|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $testRoot report.json) -Encoding utf8
    $report|ConvertTo-Json
}finally{foreach($child in $children){if(-not $child.HasExited){Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue;$child.WaitForExit()}};$env:GW_ALLOWED_TARGET_HOSTS=$oldHosts;Pop-Location}
