#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -x bin/gateway-server ]] || bash scripts/build.sh
bin/gatewayctl init --data-dir data --password-file secrets/admin-password
bin/gatewayctl provision-gateway --data-dir data --gateway gw_demo --token-file secrets/gw-demo-token
export GW_ALLOWED_TARGET_HOSTS=127.0.0.1
exec bin/gateway-server --data-dir data --listen 127.0.0.1:8080 --allow-insecure-dev
