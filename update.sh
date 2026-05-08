#!/usr/bin/env bash
set -e

APP_ROOT="/opt/crbot-project"

cd "$APP_ROOT"

git pull || true

cd "$APP_ROOT/node-admin"
npm install

cd "$APP_ROOT/go-worker"
go mod tidy
go build -o crbot-go-worker main.go

systemctl restart crbot || true
systemctl restart crbot-go-worker || true

echo "Update complete"
