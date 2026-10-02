#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"
APP_NAME="cat-state-ws"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "错误：服务器没有安装 $1。" >&2
    exit 1
  fi
}

require_command node
require_command npm
require_command pm2

EXISTING_PORT=""
EXISTING_TOKEN=""
if [[ -f "$ENV_FILE" ]]; then
  EXISTING_PORT="$(grep -m1 '^PORT=' "$ENV_FILE" | cut -d= -f2- || true)"
  EXISTING_TOKEN="$(grep -m1 '^CAT_PUBLISH_TOKEN=' "$ENV_FILE" | cut -d= -f2- || true)"
fi

PORT="${1:-${CAT_PORT:-${EXISTING_PORT:-8787}}}"
if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
  echo "错误：端口必须是 1 到 65535。" >&2
  exit 1
fi

PUBLISH_TOKEN="${CAT_PUBLISH_TOKEN:-$EXISTING_TOKEN}"
if [[ -z "$PUBLISH_TOKEN" ]]; then
  PUBLISH_TOKEN="$(node --input-type=module -e "import { randomBytes } from 'node:crypto'; console.log(randomBytes(32).toString('hex'))")"
fi
if ! [[ "$PUBLISH_TOKEN" =~ ^[A-Za-z0-9._~-]{16,256}$ ]]; then
  echo "错误：CAT_PUBLISH_TOKEN 只能包含字母、数字、点、下划线、波浪号和短横线，长度为 16 到 256。" >&2
  exit 1
fi

umask 077
{
  printf 'PORT=%s\n' "$PORT"
  printf 'CAT_PUBLISH_TOKEN=%s\n' "$PUBLISH_TOKEN"
} > "$ENV_FILE"

cd "$SCRIPT_DIR"
npm ci --omit=dev
pm2 startOrReload ecosystem.config.cjs --update-env

RUN_USER="$(id -un)"
RUN_HOME="$HOME"
SERVICE_NAME="pm2-${RUN_USER}.service"

if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
  if ! systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null; then
    echo "正在注册 PM2 开机自启，非 root 用户可能需要输入 sudo 密码……"
    if [[ "$EUID" -eq 0 ]]; then
      env PATH="$PATH" pm2 startup systemd -u "$RUN_USER" --hp "$RUN_HOME"
    elif command -v sudo >/dev/null 2>&1; then
      sudo env PATH="$PATH" pm2 startup systemd -u "$RUN_USER" --hp "$RUN_HOME"
    else
      echo "提示：没有 sudo，无法自动注册 systemd；当前服务已启动，但重启服务器后不会自动恢复。" >&2
    fi
  fi
else
  echo "提示：当前系统不是 systemd，无法自动注册开机自启；当前服务仍会由 PM2 管理。" >&2
fi

pm2 save --force

echo
echo "Cat WebSocket 服务已启动。"
echo "WebSocket：ws://服务器IP:${PORT}/ws"
echo "健康检查：http://服务器IP:${PORT}/health"
echo "发布密钥保存在：$ENV_FILE"
echo "查看日志：pm2 logs $APP_NAME"
