param(
    [ValidateSet('Start','Stop','Status')][string]$Action='Start',
    [ValidateRange(1024,65535)][int]$Port=18780,
    [string]$Instance='frontend-local',
    [switch]$SkipBuild,
    [string]$AIPythonPath='',
    [string]$AIPdfTextPath='',
    [string]$AIPdfRasterPath=''
)
$ErrorActionPreference='Stop'
$serverRoot=Split-Path -Parent $PSScriptRoot
if($Instance -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Instance must be a simple folder name.'}
$runtime=Join-Path $serverRoot ".local/$Instance"
$pidFile=Join-Path $runtime 'process.json'
$binary=Join-Path $runtime 'gateway-server.exe'
$aiToolsFile=Join-Path $runtime 'ai-tools.json'
function Existing-Process {
    if(-not(Test-Path -LiteralPath $pidFile)){return $null}
    $record=Get-Content -LiteralPath $pidFile -Raw|ConvertFrom-Json
    $proc=Get-Process -Id $record.pid -ErrorAction SilentlyContinue
    if(-not $proc){return $null}
    if([IO.Path]::GetFullPath($proc.Path) -ne [IO.Path]::GetFullPath($binary) -or $proc.StartTime.ToUniversalTime().Ticks -ne ([DateTime]$record.startedAt).ToUniversalTime().Ticks){throw 'PID was reused or points to another executable; refusing to manage it.'}
    return $proc
}
$existing=Existing-Process
if($Action -eq 'Stop'){
    if($existing){Stop-Process -Id $existing.Id; $existing.WaitForExit(); Write-Host 'Stopped this local instance. Data and credentials retained.'}else{Write-Host 'Local instance is not running.'}
    return
}
if($Action -eq 'Status'){
    if($existing){$record=Get-Content -LiteralPath $pidFile -Raw|ConvertFrom-Json;$isReady=$false;$healthError='';try{$health=Invoke-RestMethod "$($record.baseUrl)/health/ready" -TimeoutSec 3;$isReady=$health.status -eq 'ok'}catch{$healthError=$_.Exception.Message};[pscustomobject]@{running=$true;pid=$existing.Id;url="$($record.baseUrl)/ui/";ready=$isReady;healthError=$healthError;dataDirectory=(Join-Path $runtime 'data')}}else{[pscustomobject]@{running=$false;dataDirectory=(Join-Path $runtime 'data')}}
    return
}
if($existing){$record=Get-Content -LiteralPath $pidFile -Raw|ConvertFrom-Json; Write-Host "Already running: $($record.baseUrl)/ui/"; return}
$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$Port)
try{$probe.Start()}catch{throw "Loopback port $Port is occupied; select another -Port."}finally{$probe.Stop()}
if(-not $SkipBuild){& "$PSScriptRoot/build.ps1"}
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
Copy-Item -LiteralPath (Join-Path $serverRoot 'bin/gateway-server.exe') -Destination $binary -Force
Copy-Item -LiteralPath (Join-Path $serverRoot 'bin/gatewayctl.exe') -Destination (Join-Path $runtime 'gatewayctl.exe') -Force
$aiTools=@{}
if(Test-Path -LiteralPath $aiToolsFile){
    $savedTools=Get-Content -LiteralPath $aiToolsFile -Raw|ConvertFrom-Json -AsHashtable
    foreach($key in @('GW_AI_PYTHON','GW_AI_PDFTOTEXT','GW_AI_PDFTOPPM')){
        if($savedTools.ContainsKey($key)){$aiTools[$key]=[string]$savedTools[$key]}
    }
}
foreach($item in @(@('GW_AI_PYTHON',$AIPythonPath),@('GW_AI_PDFTOTEXT',$AIPdfTextPath),@('GW_AI_PDFTOPPM',$AIPdfRasterPath))){
    if($item[1]){$aiTools[$item[0]]=[string]$item[1]}
}
$previousTools=@{}
foreach($key in $aiTools.Keys){
    $toolPath=$aiTools[$key]
    if(-not [IO.Path]::IsPathFullyQualified($toolPath) -or -not(Test-Path -LiteralPath $toolPath -PathType Leaf)){throw "AI extraction tool path is not an existing absolute file: $key"}
    $previousTools[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
}
if($aiTools.Count){$aiTools|ConvertTo-Json|Set-Content -LiteralPath $aiToolsFile -Encoding utf8}
Push-Location $runtime
try{
    foreach($key in $aiTools.Keys){[Environment]::SetEnvironmentVariable($key,$aiTools[$key],'Process')}
    & './gatewayctl.exe' init --data-dir data --password-file secrets/admin-password --local-default-admin
    if($LASTEXITCODE -ne 0){throw 'Initialization failed.'}
    $proc=Start-Process -FilePath $binary -ArgumentList @('--data-dir','data','--listen',"127.0.0.1:$Port",'--allow-insecure-dev') -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runtime 'server.out.log') -RedirectStandardError (Join-Path $runtime 'server.err.log')
    $base="http://127.0.0.1:$Port"
    $ready=$false
    for($i=0;$i -lt 100;$i++){
        $proc.Refresh();if($proc.HasExited){throw "Server exited; inspect $runtime/server.err.log"}
        try{$null=Invoke-RestMethod "$base/health/ready" -TimeoutSec 1;$ready=$true;break}catch{Start-Sleep -Milliseconds 100}
    }
    if(-not $ready){Stop-Process -Id $proc.Id;throw 'Server did not become ready.'}
    @{pid=$proc.Id;startedAt=$proc.StartTime.ToUniversalTime().ToString('o');baseUrl=$base;version=(& './gateway-server.exe' --version)}|ConvertTo-Json|Set-Content -LiteralPath $pidFile -Encoding utf8
    Write-Host "Ready: $base/ui/"
    Write-Host 'Account: admin'
    Write-Host "Password file: $runtime/secrets/admin-password"
    Write-Host 'This loopback instance keeps running after this script exits. Use -Action Stop to stop it.'
}finally{
    foreach($key in $previousTools.Keys){[Environment]::SetEnvironmentVariable($key,$previousTools[$key],'Process')}
    Pop-Location
}
