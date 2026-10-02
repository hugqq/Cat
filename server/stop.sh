#!/usr/bin/env bash
set -Eeuo pipefail

APP_NAME="cat-state-ws"

if ! command -v pm2 >/dev/null 2>&1; then
  echo "错误：服务器没有安装 pm2。" >&2
  exit 1
fi

if ! pm2 describe "$APP_NAME" >/dev/null 2>&1; then
  echo "Cat WebSocket 服务当前没有运行。"
  exit 0
fi

pm2 stop "$APP_NAME"
pm2 save --force
echo "Cat WebSocket 服务已停止。"
