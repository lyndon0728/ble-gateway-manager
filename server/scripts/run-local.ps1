param([string]$Listen = '127.0.0.1:8080')
$ErrorActionPreference = 'Stop'
$serverRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $serverRoot 'bin/gateway-server.exe'))) { & "$PSScriptRoot/build.ps1" }
Push-Location $serverRoot
try {
    & './bin/gatewayctl.exe' init --data-dir data --password-file secrets/admin-password
    if ($LASTEXITCODE -ne 0) { throw 'Initialization failed.' }
    & './bin/gatewayctl.exe' provision-gateway --data-dir data --gateway gw_demo --token-file secrets/gw-demo-token
    if ($LASTEXITCODE -ne 0) { throw 'Gateway provisioning failed.' }
    $env:GW_ALLOWED_TARGET_HOSTS = '127.0.0.1'
    Write-Host 'Local evaluation mode. Credentials are stored in server/secrets, never printed here.'
    & './bin/gateway-server.exe' --data-dir data --listen $Listen --allow-insecure-dev
    if ($LASTEXITCODE -ne 0) { throw 'Server failed.' }
} finally { Pop-Location }
