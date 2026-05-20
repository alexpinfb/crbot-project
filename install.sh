#!/usr/bin/env bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"

ask() {
  local name="$1"
  local prompt="$2"
  local value=""
  read -r -p "$prompt: " value
  printf '%s' "$value"
}

echo "CRBOT installer"
echo
echo "Choose install type:"
echo "1) admin"
echo "2) worker"
read -r -p "Type 1 or 2: " TYPE

if [ "$TYPE" = "1" ] || [ "$TYPE" = "admin" ]; then
  cd "$REPO_DIR/admin"

  apt update
  apt install -y nodejs npm redis-server git

  npm install

  BOT_TOKEN="$(ask BOT_TOKEN 'Telegram BOT_TOKEN')"
  CHAT_ID="$(ask CHAT_ID 'Telegram CHAT_ID')"
  COOKIE="$(ask COOKIE 'Account COOKIE')"
  USER_AGENT="$(ask USER_AGENT 'Browser USER_AGENT')"

  cat > .env <<ENV
BOT_TOKEN=$BOT_TOKEN
CHAT_ID=$CHAT_ID

REDIS_URL=redis://127.0.0.1:6379
WORKER_ID=v1admin
ACCOUNT_NAME=Main

COOKIE=$COOKIE
USER_AGENT=$USER_AGENT

PROVIDER=nspk
RECONNECT_MS=5000
DEFAULT_METHOD=AUTO
ENV

  cat > /etc/systemd/system/crbot-admin.service <<SERVICE
[Unit]
Description=CRBot Admin Bot
After=network.target redis-server.service

[Service]
WorkingDirectory=$REPO_DIR/admin
ExecStart=/usr/bin/node $REPO_DIR/admin/bot.js
Restart=always
RestartSec=3
Environment=NODE_ENV=production

[Install]
WantedBy=multi-user.target
SERVICE

  systemctl daemon-reload
  systemctl enable crbot-admin
  systemctl restart crbot-admin

  echo
  echo "ADMIN installed."
  echo "Check:"
  echo "systemctl status crbot-admin --no-pager"
  echo "journalctl -u crbot-admin -f"

elif [ "$TYPE" = "2" ] || [ "$TYPE" = "worker" ]; then
  cd "$REPO_DIR/worker"

  apt update
  apt install -y golang-go ca-certificates git

  REDIS_URL="$(ask REDIS_URL 'REDIS_URL, example redis://ADMIN_IP:6379')"
  WORKER_ID="$(ask WORKER_ID 'WORKER_ID, example a1w1')"
  COOKIE="$(ask COOKIE 'Account COOKIE')"
  USER_AGENT="$(ask USER_AGENT 'Browser USER_AGENT')"

  cat > .env <<ENV
REDIS_URL=$REDIS_URL
WORKER_ID=$WORKER_ID

COOKIE=$COOKIE
USER_AGENT=$USER_AGENT

PROVIDER=nspk
ENV

  go build -o crbot-go-worker main.go

  cat > /etc/systemd/system/crbot-go-worker.service <<SERVICE
[Unit]
Description=CR Bot Go Worker
After=network.target

[Service]
WorkingDirectory=$REPO_DIR/worker
ExecStart=$REPO_DIR/worker/crbot-go-worker
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
SERVICE

  systemctl daemon-reload
  systemctl enable crbot-go-worker
  systemctl restart crbot-go-worker

  echo
  echo "WORKER installed."
  echo "Check:"
  echo "systemctl status crbot-go-worker --no-pager"
  echo "tail -f $REPO_DIR/worker/live-go.log"

else
  echo "Unknown type: $TYPE"
  exit 1
fi
