#!/usr/bin/env bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_DIR"

echo "Updating CRBOT..."
git pull --ff-only

if [ -d "$REPO_DIR/admin" ] && [ -f "$REPO_DIR/admin/package.json" ]; then
  echo "Updating admin dependencies..."
  cd "$REPO_DIR/admin"
  npm install
fi

if [ -d "$REPO_DIR/worker" ] && [ -f "$REPO_DIR/worker/main.go" ]; then
  echo "Building worker..."
  cd "$REPO_DIR/worker"
  go build -o crbot-go-worker main.go
fi

systemctl daemon-reload

if systemctl list-unit-files | grep -q '^crbot-admin.service'; then
  systemctl restart crbot-admin || true
fi

if systemctl list-unit-files | grep -q '^crbot-go-worker.service'; then
  systemctl restart crbot-go-worker || true
fi

echo "Update done."
