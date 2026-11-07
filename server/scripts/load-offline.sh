#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v docker >/dev/null
sha256sum --check SHA256SUMS
docker load -i images.tar
printf '%s\n' 'Images loaded. Supply deploy/tls files, review deploy/.env, then run sudo bash scripts/install-linux.sh.'
