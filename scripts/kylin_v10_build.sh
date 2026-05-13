#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "${ROOT_DIR}/.env.kylin" ]] || { echo "缺少 .env.kylin，请先执行 scripts/kylin_v10_install_deps.sh" >&2; exit 1; }
# shellcheck disable=SC1091
source "${ROOT_DIR}/.env.kylin"

JOBS="${JOBS:-$(nproc)}"

echo "[kylin-build] 构建 C++ 后端"
cmake -S "${ROOT_DIR}/backend" -B "${ROOT_DIR}/backend/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}"
cmake --build "${ROOT_DIR}/backend/build" -j"${JOBS}"

echo "[kylin-build] 安装/校验前端依赖"
(cd "${ROOT_DIR}/frontend" && npm ci)

echo "[kylin-build] 前端生产构建校验"
(cd "${ROOT_DIR}/frontend" && npm run build)

echo "[kylin-build] 完成"
