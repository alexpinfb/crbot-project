#!/usr/bin/env bash

echo "=== Node admin logs ==="
journalctl -u crbot -n 50 --no-pager

echo
echo "=== Go worker logs ==="
journalctl -u crbot-go-worker -n 50 --no-pager
