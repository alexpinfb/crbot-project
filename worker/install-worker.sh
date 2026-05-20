#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

apt update
apt install -y golang-go ca-certificates

if [ ! -f .env ]; then
  cp .env.example .env
  echo "Created .env. Edit it before starting."
fi

go build -o crbot-go-worker main.go

cat > /etc/systemd/system/crbot-go-worker.service <<SERVICE
[Unit]
Description=CR Bot Go Worker
After=network.target

[Service]
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/crbot-go-worker
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable crbot-go-worker

echo "Edit $(pwd)/.env, then run:"
echo "systemctl restart crbot-go-worker"
