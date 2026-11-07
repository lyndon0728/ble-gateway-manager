param([switch]$SkipBuild, [int]$ServerPort=18080, [int]$CustomerPort=19090)
$ErrorActionPreference='Stop'
$serverRoot=Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) { & "$PSScriptRoot/build.ps1" }
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('ble-gateway-e2e-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$binary=Join-Path $serverRoot 'bin'
$children=[Collections.Generic.List[Diagnostics.Process]]::new()
$oldHosts=$env:GW_ALLOWED_TARGET_HOSTS
$base="http://127.0.0.1:$ServerPort"
$customer="http://127.0.0.1:$CustomerPort"
foreach($port in @($ServerPort,$CustomerPort)) {
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$port)
    try { $listener.Start() } catch { throw "Test port $port is in use; select different -ServerPort/-CustomerPort values." } finally { $listener.Stop() }
}
function Start-Child([string]$Name,[string[]]$Arguments) {
    $process=Start-Process -FilePath (Join-Path $binary "$Name.exe") -ArgumentList $Arguments -WorkingDirectory $testRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $testRoot "$Name.out.log") -RedirectStandardError (Join-Path $testRoot "$Name.err.log")
    $children.Add($process)
    return $process
}
function Wait-HTTP([string]$Url) {
    for ($i=0;$i -lt 100;$i++) { foreach($child in $children){if($child.HasExited){throw "Test process exited early: $($child.ProcessName). See $testRoot logs."}}; try { $null=Invoke-RestMethod $Url -TimeoutSec 1; return } catch { Start-Sleep -Milliseconds 100 } }
    throw "Endpoint not ready: $Url"
}
function API([string]$Method,[string]$Path,$Body=$null,[hashtable]$Extra=@{}) {
    $headers=@{'X-CSRF-Token'=$script:csrf;'Idempotency-Key'=[Guid]::NewGuid().ToString('N')}
    foreach($k in $Extra.Keys){$headers[$k]=$Extra[$k]}
    $request=@{Method=$Method;Uri="$base$Path";WebSession=$script:session;Headers=$headers;TimeoutSec=10}
    if($null -ne $Body){$request.ContentType='application/json';$request.Body=ConvertTo-Json -InputObject $Body -Depth 30 -Compress}
    return Invoke-RestMethod @request
}
Push-Location $testRoot
try {
    & "$binary/gatewayctl.exe" init --data-dir data --password-file secrets/admin-password
    if($LASTEXITCODE -ne 0){throw 'init failed'}
    & "$binary/gatewayctl.exe" init --data-dir data --password-file secrets/admin-password
    if($LASTEXITCODE -ne 0){throw 'idempotent init failed'}
    & "$binary/gatewayctl.exe" provision-gateway --data-dir data --gateway gw_demo --token-file secrets/gateway-token
    if($LASTEXITCODE -ne 0){throw 'provision failed'}
    $env:GW_ALLOWED_TARGET_HOSTS='127.0.0.1'
    $server=Start-Child gateway-server @('--data-dir','data','--listen',"127.0.0.1:$ServerPort",'--allow-insecure-dev')
    Wait-HTTP "$base/health/ready"
    $script:session=[Microsoft.PowerShell.Commands.WebRequestSession]::new()
    $login=Invoke-RestMethod -Method Post -Uri "$base/api/v1/auth/sessions" -WebSession $script:session -ContentType application/json -Body (@{name='admin';password=(Get-Content -LiteralPath 'secrets/admin-password' -Raw).Trim()}|ConvertTo-Json -Compress)
    $script:csrf=$login.csrfToken
    $target=API POST '/api/v1/targets' @{name='durable customer simulator';configuration=@{protocol='HTTP';endpoint="$customer/events";ackMode='HTTP_DURABLE_RESPONSE';maxBatchEvents=200;maxInflightEvents=200;maxQueueBytes=10485760;mappingVersion='identity-1'}}
    $receiver=Start-Child customer-simulator @('--listen',"127.0.0.1:$CustomerPort",'--db','customer.db','--target',$target.targetId,'--target-version','1','--drop-acks','1')
    Wait-HTTP "$customer/health/live"
    $bundle=API POST '/api/v1/rule-bundles' @{name='reference sensor route';defaultAction='QUARANTINE';rules=@(@{ruleId='reference_rule';priority=100;match=@{modelIds=@('REFERENCE_GW');protocolVersions=@('reference-sensor-v1')};action='DECODE';parserId='reference_sensor';parserVersion='1.0.0';schemaVersion='reference-sensor-1';targetVersions=@(@{targetId=$target.targetId;version='1';mappingVersion='identity-1';required=$true})})}
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/validations" @{}
    $status=API GET '/api/v1/system/status'
    $node=$status.dataNodes[0]
    $null=API POST "/api/v1/rule-bundles/$($bundle.bundleId)/activations" @{dataNodeId=$node.dataNodeId;expectedActivationRevision=$node.activationRevision} @{'If-Match'=$node.activationEtag}
    $task=API POST '/api/v1/tasks' @{gatewayId='gw_demo';type='REBOOT';parameters=@{reason='simulator durability test'};expiresAt=[DateTime]::UtcNow.AddMinutes(2).ToString('o')}
    $gateway=Start-Child gateway-simulator @('--server',$base,'--db','gateway.db','--token-file','secrets/gateway-token','--gateway','gw_demo','--count','10','--duration','12s','--offline','1s','--ignore-acks','1','--interval','300ms')
    $deadline=[DateTime]::UtcNow.AddSeconds(25)
    do {
        Start-Sleep -Milliseconds 300
        $stats=Invoke-RestMethod "$customer/stats"
        $result=API GET "/api/v1/tasks/$($task.taskId)"
    } while (($stats.businessRecords -ne 10 -or $stats.requests -lt 2 -or $result.state -ne 'SUCCEEDED') -and [DateTime]::UtcNow -lt $deadline)
    if($stats.businessRecords -ne 10 -or $stats.deliveries -ne 10 -or $stats.requests -lt 2){throw "Reliable delivery assertion failed: $($stats|ConvertTo-Json -Compress)"}
    if($result.state -ne 'SUCCEEDED' -or $result.result.verification -ne 'VERIFIED'){throw 'Downlink task failed to reach verified success'}
    $gateway.WaitForExit(15000)|Out-Null
    if(-not $gateway.HasExited){throw 'Gateway simulator did not finish'}
    Stop-Process -Id $server.Id -Force
    $server.WaitForExit()
    & "$binary/gatewayctl.exe" backup --data-dir data --out backup.bgb --key-file secrets/backup-key
    if($LASTEXITCODE -ne 0){throw 'Backup failed'}
    & "$binary/gatewayctl.exe" restore --data-dir restored --archive backup.bgb --key-file secrets/backup-key
    if($LASTEXITCODE -ne 0){throw 'Restore failed'}
    if(-not(Test-Path -LiteralPath 'restored/restore-reconciliation.required')){throw 'Restore did not lock revision reconciliation'}
    $report=@{passed=$true;at=[DateTime]::UtcNow.ToString('o');transport='HTTP loopback';newEvents=10;customerDeliveries=$stats.deliveries;customerBusinessRecords=$stats.businessRecords;customerRequests=$stats.requests;taskState=$result.state;taskVerification=$result.result.verification;backupRestore='passed';evidenceDirectory=$testRoot}
    $report|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $testRoot 'report.json') -Encoding utf8
    $report|ConvertTo-Json
} finally {
    foreach($child in $children){if(-not $child.HasExited){Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue; $child.WaitForExit()}}
    $env:GW_ALLOWED_TARGET_HOSTS=$oldHosts
    Pop-Location
}
