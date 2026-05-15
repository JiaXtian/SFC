#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "${ROOT_DIR}/.env.kylin" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.kylin"
fi

"${ROOT_DIR}/dev.sh" status
echo
echo "[kylin-status] Docker containers"
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}' | grep -E 'NAMES|sfc-|mysql|mongo|open5gs|ueransim' || true
else
  echo "Docker 不可用或当前用户无权限"
fi
echo
echo "[kylin-status] Backend health"
if command -v curl >/dev/null 2>&1; then
  curl -fsS http://127.0.0.1:8080/api/v1/health || true
  echo
else
  echo "curl not found"
fi
