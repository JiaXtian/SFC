#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "${ROOT_DIR}/.env.kylin" ]] || { echo "缺少 .env.kylin，请先执行 scripts/kylin_v10_install_deps.sh" >&2; exit 1; }
# shellcheck disable=SC1091
source "${ROOT_DIR}/.env.kylin"

if ! docker info >/dev/null 2>&1; then
  echo "当前用户无法访问 Docker。请先启动 Docker 并确认 docker 组已生效。" >&2
  exit 1
fi

"${ROOT_DIR}/dev.sh" start

HOST_IP="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
echo
echo "访问地址："
echo "  大屏主页面:     http://${HOST_IP:-127.0.0.1}:3001"
echo "  系统控制中心:   http://${HOST_IP:-127.0.0.1}:3002"
echo "  后端健康检查:   http://${HOST_IP:-127.0.0.1}:8080/api/v1/health"
