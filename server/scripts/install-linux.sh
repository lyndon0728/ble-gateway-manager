#!/usr/bin/env bash
# Run as root on the customer Linux host after placing customer-issued TLS files.
set -euo pipefail
cd "$(dirname "$0")/../deploy"
[[ "$(id -u)" == 0 ]] || { echo 'Run this installer as root to set numeric container ownership.' >&2; exit 1; }
for tool in docker openssl df; do command -v "$tool" >/dev/null || { echo "Missing prerequisite: $tool" >&2; exit 1; }; done
docker info >/dev/null
docker compose version >/dev/null
for file in ca.crt server.crt server.key broker.crt broker.key; do
  [[ -s "tls/$file" ]] || { echo "Supply customer TLS file deploy/tls/$file before installation." >&2; exit 1; }
done
openssl verify -CAfile tls/ca.crt tls/server.crt tls/broker.crt >/dev/null
openssl x509 -in tls/server.crt -checkend 86400 -noout >/dev/null
openssl x509 -in tls/broker.crt -checkhost broker -noout >/dev/null
for prefix in server broker; do
  cert_public=$(openssl x509 -in "tls/$prefix.crt" -pubkey -noout | openssl sha256)
  key_public=$(openssl pkey -in "tls/$prefix.key" -pubout | openssl sha256)
  [[ "$cert_public" == "$key_public" ]] || { echo "$prefix certificate/key mismatch." >&2; exit 1; }
done
[[ -f .env ]] || cp .env.example .env
docker compose config --quiet
available_kb=$(df -Pk . | awk 'NR==2 {print $4}')
[[ "$available_kb" -ge 5242880 ]] || { echo 'At least 5 GiB available disk is required for this starter profile.' >&2; exit 1; }
install -d -m 0700 -o 10001 -g 10001 data secrets
install -d -m 0700 -o 10001 -g 10001 data/tmp
install -d -m 0700 -o 1883 -g 1883 mosquitto/data
chmod 0755 tls mosquitto
chmod 0644 tls/*.crt mosquitto/acl mosquitto/mosquitto.conf
chown 10001:10001 tls/server.key
chown 1883:1883 tls/broker.key
chmod 0400 tls/server.key tls/broker.key
umask 077
for name in admin-password broker-password; do
  if [[ ! -f "secrets/$name" ]]; then openssl rand -hex 32 > "secrets/$name"; fi
  chown 10001:10001 "secrets/$name"
  chmod 0400 "secrets/$name"
done
if [[ "${1:-}" == --build ]]; then docker compose build; fi
if [[ ! -f mosquitto/passwords ]]; then
  # mosquitto_passwd -U converts the temporary plaintext file in place. No secret
  # is passed as a process argument or printed into installer/container logs.
  broker_password=$(cat secrets/broker-password)
  password_stage=$(mktemp "$PWD/mosquitto/.passwords.XXXXXX")
  trap 'rm -f -- "$password_stage"' EXIT
  printf 'platform:%s\n' "$broker_password" > "$password_stage"
  unset broker_password
  broker_image=${MOSQUITTO_IMAGE:-$(sed -n 's/^MOSQUITTO_IMAGE=//p' .env | tail -1)}
  [[ -n "$broker_image" ]] || broker_image=eclipse-mosquitto:2.0.22
  docker run --rm --user 0:0 --entrypoint mosquitto_passwd -v "$PWD/mosquitto:/config" "$broker_image" -U "/config/$(basename "$password_stage")"
  chown 1883:1883 "$password_stage"
  chmod 0400 "$password_stage"
  mv -T "$password_stage" mosquitto/passwords
  trap - EXIT
fi
chown 1883:1883 mosquitto/passwords
chmod 0400 mosquitto/passwords
docker compose up -d
printf '%s\n' 'Started private deployment. Review docker compose ps and health checks before acceptance.'
