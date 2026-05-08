#!/usr/bin/env bash
set -e

PROJECT_DIR="/opt/crbot-project"

echo "=== CRBOT REMOTE UPDATE ==="

if [ ! -d "$PROJECT_DIR/.git" ]; then
  echo "Project not found: $PROJECT_DIR"
  exit 1
fi

cd "$PROJECT_DIR"

echo "=== Git pull ==="
git pull

echo "=== Update node-admin ==="
cd node-admin
npm install

echo "=== Build go-worker ==="
cd ../go-worker
go build -o crbot-go-worker main.go license.go

echo "=== Restart services ==="
systemctl restart crbot || true
systemctl restart crbot-go-worker || true

echo "=== DONE ==="
