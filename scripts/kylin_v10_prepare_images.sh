#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "${ROOT_DIR}/.env.kylin" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.kylin"
fi

OPEN5GS_REF="${OPEN5GS_REF:-v2.7.7}"
UBUNTU_MIRROR="${UBUNTU_MIRROR:-http://archive.ubuntu.com/ubuntu}"
SAT_IMAGE="${SFC_SATELLITE_IMAGE_AMD64:-docker.1ms.run/gradiant/open5gs:2.7.7}"

log() { printf '\033[1;36m[kylin-images]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[kylin-images]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[kylin-images]\033[0m %s\n' "$*" >&2; exit 1; }

docker_ready() {
  docker info >/dev/null 2>&1
}

main() {
  command -v docker >/dev/null 2>&1 || die "未找到 docker"
  if ! docker_ready; then
    die "当前用户无法访问 Docker。请确认 Docker 已启动，并重新登录使 docker 组生效：sudo systemctl enable --now docker && sudo usermod -aG docker $USER"
  fi

  docker network inspect sfc-net >/dev/null 2>&1 || docker network create sfc-net >/dev/null
  docker network inspect sfc-open5gs-net >/dev/null 2>&1 || docker network create sfc-open5gs-net >/dev/null

  log "拉取基础镜像 mysql:8.0 / mongo:6 / UERANSIM"
  docker pull mysql:8.0
  docker pull mongo:6
  docker pull "${UERANSIM_IMAGE:-docker.io/free5gc/ueransim:latest}" || warn "UERANSIM 镜像拉取失败，功能验证时后端会再次尝试"

  if docker image inspect "${SAT_IMAGE}" >/dev/null 2>&1; then
    log "Open5GS 镜像已存在: ${SAT_IMAGE}"
    return 0
  fi

  log "拉取 Open5GS 镜像: ${SAT_IMAGE}"
  if docker pull "${SAT_IMAGE}"; then
    return 0
  fi

  warn "拉取 ${SAT_IMAGE} 失败，改为从 docker/open5gs-satellite.Dockerfile 本地构建"
  docker build \
    --build-arg UBUNTU_MIRROR="${UBUNTU_MIRROR}" \
    --build-arg OPEN5GS_REF="${OPEN5GS_REF}" \
    -f "${ROOT_DIR}/docker/open5gs-satellite.Dockerfile" \
    -t sfc-open5gs-satellite:local \
    "${ROOT_DIR}"
}

main "$@"
