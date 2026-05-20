#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

apt update
apt install -y nodejs npm redis-server

npm install

if [ ! -f .env ]; then
  cp .env.example .env
  echo "Created .env. Edit it before starting."
fi

cat > /etc/systemd/system/crbot-admin.service <<SERVICE
[Unit]
Description=CRBot Admin Bot
After=network.target redis-server.service

[Service]
WorkingDirectory=$(pwd)
ExecStart=/usr/bin/node $(pwd)/bot.js
Restart=always
RestartSec=3
Environment=NODE_ENV=production

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable crbot-admin

echo "Edit $(pwd)/.env, then run:"
echo "systemctl restart crbot-admin"
