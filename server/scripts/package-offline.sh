#!/usr/bin/env bash
# Run on a connected Linux build host with Docker. Output must be a new path.
set -euo pipefail
server_root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?Usage: package-offline.sh NEW_OUTPUT_DIRECTORY}
image=${GW_IMAGE:-ble-gateway-server:0.2.0}
broker=${MOSQUITTO_IMAGE:-eclipse-mosquitto:2.0.22}
[[ ! -e "$output" ]] || { echo 'Output directory must not exist.' >&2; exit 1; }
mkdir -p "$output/deploy/mosquitto" "$output/scripts" "$output/docs" "$output/contracts"
output=$(cd "$output" && pwd)
docker build --pull -f "$server_root/deploy/Dockerfile" -t "$image" "$server_root"
docker pull "$broker"
docker save -o "$output/images.tar" "$image" "$broker"
cp "$server_root/deploy/compose.yaml" "$server_root/deploy/.env.example" "$output/deploy/"
cp "$server_root/deploy/mosquitto/mosquitto.conf" "$server_root/deploy/mosquitto/acl" "$output/deploy/mosquitto/"
cp "$server_root/scripts/install-linux.sh" "$server_root/scripts/load-offline.sh" "$output/scripts/"
cp "$server_root/README.md" "$output/README.md"
cp "$server_root/THIRD_PARTY_NOTICES.md" "$server_root/dependencies.json" "$output/"
cp -R "$server_root/licenses" "$output/licenses"
cp "$server_root/docs/"*.md "$output/docs/"
cp "$server_root/internal/platform/assets/openapi.json" "$server_root/internal/platform/assets/message-contracts.schema.json" "$server_root/internal/platform/assets/mqtt-contracts.json" "$output/contracts/"
printf 'GW_IMAGE=%s\nMOSQUITTO_IMAGE=%s\n' "$image" "$broker" > "$output/deploy/.env"
docker image inspect --format '{{.Id}} {{json .RepoDigests}}' "$image" "$broker" > "$output/IMAGE-IDENTITIES.txt"
cd "$output"
find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
printf 'Prepared offline image bundle in %s. Customer credentials/certificates are excluded.\n' "$output"
