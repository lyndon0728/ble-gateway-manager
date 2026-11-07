param([string]$Go = 'go', [switch]$Test)
$ErrorActionPreference = 'Stop'
$serverRoot = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command $Go -ErrorAction SilentlyContinue)) {
    $bundledGo = Join-Path (Split-Path -Parent $serverRoot) '.tools/go/bin/go.exe'
    if (Test-Path -LiteralPath $bundledGo) { $Go = $bundledGo } else { throw 'Go from go.mod is required.' }
}
Push-Location $serverRoot
try {
    if ($Test) {
        & $Go test ./...
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
        & $Go vet ./...
        if ($LASTEXITCODE -ne 0) { throw 'go vet failed.' }
    }
    New-Item -ItemType Directory -Force -Path bin | Out-Null
    foreach ($name in @('gateway-server', 'gatewayctl', 'gateway-simulator', 'customer-simulator')) {
        & $Go build -trimpath -o "bin/$name.exe" "./cmd/$name"
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
    }
    Write-Host 'Built four server and verification executables in server/bin.'
} finally { Pop-Location }
