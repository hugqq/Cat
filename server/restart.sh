#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"

for command_name in node npm pm2; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "错误：服务器没有安装 $command_name。" >&2
    exit 1
  fi
done

if [[ ! -f "$ENV_FILE" ]]; then
  echo "错误：没有找到 $ENV_FILE，请先运行 start.sh。" >&2
  exit 1
fi

cd "$SCRIPT_DIR"
npm ci --omit=dev
pm2 startOrReload ecosystem.config.cjs --update-env
pm2 save --force

echo "Cat WebSocket 服务已重启。"
