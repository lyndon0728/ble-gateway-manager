#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v go >/dev/null || { echo 'Go from go.mod is required.' >&2; exit 1; }
if [[ "${1:-}" == --test ]]; then go test ./...; go vet ./...; fi
mkdir -p bin
for name in gateway-server gatewayctl gateway-simulator customer-simulator; do
  go build -trimpath -o "bin/$name" "./cmd/$name"
done
printf '%s\n' 'Built four executables in server/bin.'
