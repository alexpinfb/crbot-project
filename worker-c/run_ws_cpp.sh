#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

set -a
source ./.env
set +a

export CRBOT_COOKIE="$(
  redis-cli --raw --no-auth-warning \
    -h "$REDIS_ADDR" \
    -p "${REDIS_PORT:-6379}" \
    -a "$REDIS_PASSWORD" \
    GET "crbot:account:${ACCOUNT_ID}:cookie"
)"

export CRBOT_UA="$(
  redis-cli --raw --no-auth-warning \
    -h "$REDIS_ADDR" \
    -p "${REDIS_PORT:-6379}" \
    -a "$REDIS_PASSWORD" \
    GET "crbot:account:${ACCOUNT_ID}:userAgent"
)"

echo "RUN_WS_CPP WORKER_ID=${WORKER_ID:-unknown} ACCOUNT_ID=${ACCOUNT_ID:-unknown} cookieLen=${#CRBOT_COOKIE} uaLen=${#CRBOT_UA}"

while true; do
  ./ws_cpp
  echo "RUN_WS_CPP_RESTART sleep=1"
  sleep 1
done
