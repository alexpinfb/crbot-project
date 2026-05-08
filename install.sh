#!/usr/bin/env bash
set -e

APP_ROOT="/opt/crbot-project"
NODE_DIR="$APP_ROOT/node-admin"
GO_DIR="$APP_ROOT/go-worker"

echo "=== Install packages ==="
apt update
apt install -y curl ca-certificates gnupg git redis-tools wget tar

echo "=== Install Node.js 20 ==="
if ! command -v node >/dev/null 2>&1; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt install -y nodejs
fi

echo "=== Install Go ==="
if ! command -v go >/dev/null 2>&1; then
  wget -q https://go.dev/dl/go1.22.12.linux-amd64.tar.gz -O /tmp/go.tar.gz
  rm -rf /usr/local/go
  tar -C /usr/local -xzf /tmp/go.tar.gz
  ln -sf /usr/local/go/bin/go /usr/local/bin/go
fi

echo "=== Prepare env files ==="
[ -f "$NODE_DIR/.env" ] || cp "$NODE_DIR/.env.example" "$NODE_DIR/.env"
[ -f "$GO_DIR/.env" ] || cp "$GO_DIR/.env.example" "$GO_DIR/.env"

echo "=== Install node-admin ==="
cd "$NODE_DIR"
npm install

echo "=== Build go-worker ==="
cd "$GO_DIR"
go mod tidy
go build -o crbot-go-worker main.go

echo "=== Create systemd: crbot ==="
cat > /etc/systemd/system/crbot.service <<SERVICE
[Unit]
Description=CRBOT Node Admin
After=network.target

[Service]
Type=simple
WorkingDirectory=$NODE_DIR
ExecStart=/usr/bin/node $NODE_DIR/bot.js
Restart=always
RestartSec=3
User=root
Environment=NODE_ENV=production
StandardOutput=append:$NODE_DIR/live-admin.log
StandardError=append:$NODE_DIR/live-admin.log

[Install]
WantedBy=multi-user.target
SERVICE

echo "=== Create systemd: crbot-go-worker ==="
cat > /etc/systemd/system/crbot-go-worker.service <<SERVICE
[Unit]
Description=CRBOT Go Worker
After=network.target

[Service]
Type=simple
WorkingDirectory=$GO_DIR
ExecStart=$GO_DIR/crbot-go-worker
Restart=always
RestartSec=3
User=root
StandardOutput=append:$GO_DIR/live-go.log
StandardError=append:$GO_DIR/live-go.log

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable crbot
systemctl enable crbot-go-worker

echo
echo "=== DONE ==="
echo "Edit env files before start:"
echo "  nano $NODE_DIR/.env"
echo "  nano $GO_DIR/.env"
echo
echo "Start:"
echo "  systemctl restart crbot"
echo "  systemctl restart crbot-go-worker"
