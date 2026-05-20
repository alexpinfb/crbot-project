#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

apt update
apt install -y ca-certificates curl tar

GO_VERSION="1.22.12"
ARCH="$(dpkg --print-architecture)"
case "$ARCH" in
  amd64) GO_ARCH="amd64" ;;
  arm64) GO_ARCH="arm64" ;;
  *) echo "Unsupported arch: $ARCH"; exit 1 ;;
esac

if ! /usr/local/go/bin/go version 2>/dev/null | grep -q "go$GO_VERSION"; then
  rm -rf /usr/local/go
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" -o /tmp/go.tar.gz
  tar -C /usr/local -xzf /tmp/go.tar.gz
fi

export PATH="/usr/local/go/bin:$PATH"

if [ ! -f .env ]; then
  cp .env.example .env
  echo "Created .env. Edit it before starting."
fi

/usr/local/go/bin/go build -o crbot-go-worker main.go

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
