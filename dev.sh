#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="${ROOT_DIR}/.run"
PID_DIR="${RUNTIME_DIR}/pids"
LOG_DIR="${RUNTIME_DIR}/logs"
PODMAN_DATA_DIR="${RUNTIME_DIR}/podman"

MYSQL_CONTAINER="sfc-mysql"
PODMAN_NETWORK="sfc-net"
MYSQL_ROOT_PASSWORD="root123456"
MYSQL_DB="sfc_runtime"
MYSQL_USER="sfc"
MYSQL_PASSWORD="sfc123456"
MYSQL_IMAGE="${MYSQL_IMAGE:-mysql:8.0}"
MYSQL_IMAGE_CANDIDATES="${MYSQL_IMAGE_CANDIDATES:-${MYSQL_IMAGE} docker.io/library/mysql:8.0 docker.m.daocloud.io/library/mysql:8.0 registry.cn-hangzhou.aliyuncs.com/library/mysql:8.0}"

BACKEND_BUILD_DIR="${ROOT_DIR}/backend/build"
BACKEND_BIN="${BACKEND_BUILD_DIR}/sfc_server"
BACKEND_PID_FILE="${PID_DIR}/backend.pid"
FRONTEND_PID_FILE="${PID_DIR}/frontend.pid"
BACKEND_LOG_FILE="${LOG_DIR}/backend.log"
FRONTEND_LOG_FILE="${LOG_DIR}/frontend.log"

mkdir -p "${PID_DIR}" "${LOG_DIR}" "${PODMAN_DATA_DIR}/mysql"

is_running() {
  local pid_file="$1"
  if [[ ! -f "${pid_file}" ]]; then
    return 1
  fi
  local pid
  pid="$(cat "${pid_file}" 2>/dev/null || true)"
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

stop_one() {
  local name="$1"
  local pid_file="$2"
  if ! is_running "${pid_file}"; then
    rm -f "${pid_file}"
    echo "${name} not running"
    return 0
  fi

  local pid
  pid="$(cat "${pid_file}")"
  kill "${pid}" 2>/dev/null || true
  for _ in {1..20}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      rm -f "${pid_file}"
      echo "stopped ${name} (pid=${pid})"
      return 0
    fi
    sleep 0.2
  done

  kill -9 "${pid}" 2>/dev/null || true
  rm -f "${pid_file}"
  echo "force stopped ${name} (pid=${pid})"
}

build_backend() {
  echo "building backend..."
  cmake -S "${ROOT_DIR}/backend" -B "${BACKEND_BUILD_DIR}"
  cmake --build "${BACKEND_BUILD_DIR}" -j
}

ensure_podman() {
  if command -v podman >/dev/null 2>&1; then
    return 0
  fi
  echo "podman not found, satellite pod control is unavailable in script layer" >&2
  return 1
}

ensure_docker() {
  if command -v docker >/dev/null 2>&1; then
    return 0
  fi
  echo "docker not found, mysql container cannot be managed" >&2
  return 1
}

ensure_network() {
  if ! ensure_docker; then
    return 1
  fi
  if ! docker network inspect "${PODMAN_NETWORK}" >/dev/null 2>&1; then
    docker network create "${PODMAN_NETWORK}" >/dev/null
  fi
}

try_pull_image() {
  local image="$1"
  echo "trying image: ${image}"
  if docker image inspect "${image}" >/dev/null 2>&1; then
    echo "image already exists locally: ${image}"
    return 0
  fi
  if docker pull "${image}" >/dev/null 2>&1; then
    echo "image pulled: ${image}"
    return 0
  fi
  echo "image pull failed: ${image}" >&2
  return 1
}

resolve_mysql_image() {
  local chosen=""
  local candidate
  for candidate in ${MYSQL_IMAGE_CANDIDATES}; do
    if try_pull_image "${candidate}"; then
      chosen="${candidate}"
      break
    fi
  done
  if [[ -z "${chosen}" ]]; then
    echo "failed to pull any mysql image candidate: ${MYSQL_IMAGE_CANDIDATES}" >&2
    return 1
  fi
  echo "${chosen}"
}

start_mysql() {
  if ! ensure_docker; then
    return 1
  fi
  ensure_network
  if docker container inspect "${MYSQL_CONTAINER}" >/dev/null 2>&1; then
    docker start "${MYSQL_CONTAINER}" >/dev/null 2>&1 || true
    return 0
  fi
  local mysql_image
  mysql_image="$(resolve_mysql_image)" || return 1
  echo "starting mysql container..."
  local mysql_data_mount="${PODMAN_DATA_DIR}/mysql:/var/lib/mysql"
  docker run -d \
    --name "${MYSQL_CONTAINER}" \
    --network "${PODMAN_NETWORK}" \
    -p 3306:3306 \
    -e MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD}" \
    -e MYSQL_DATABASE="${MYSQL_DB}" \
    -e MYSQL_USER="${MYSQL_USER}" \
    -e MYSQL_PASSWORD="${MYSQL_PASSWORD}" \
    -v "${mysql_data_mount}" \
    "${mysql_image}" \
    --character-set-server=utf8mb4 \
    --collation-server=utf8mb4_unicode_ci >/dev/null
}

wait_mysql_ready() {
  if ! ensure_docker; then
    return 1
  fi
  echo "waiting mysql ready..."
  for _ in {1..60}; do
    if docker exec "${MYSQL_CONTAINER}" mysql -u"${MYSQL_USER}" -p"${MYSQL_PASSWORD}" -e "SELECT 1;" >/dev/null 2>&1; then
      echo "mysql ready"
      return 0
    fi
    sleep 2
  done
  echo "mysql not ready after timeout" >&2
  return 1
}

start_infra() {
  start_mysql
  wait_mysql_ready
}

stop_satellite_nodes() {
  if ! ensure_podman; then
    return 0
  fi
  local ids
  ids="$(podman ps -a --filter label=sfc.satellite=true -q 2>/dev/null || true)"
  if [[ -n "${ids}" ]]; then
    echo "stopping satellite node containers..."
    podman stop ${ids} >/dev/null 2>&1 || true
  fi
}

stop_infra() {
  stop_satellite_nodes
  if ensure_docker; then
    docker stop "${MYSQL_CONTAINER}" >/dev/null 2>&1 || true
  fi
}

start_backend() {
  if is_running "${BACKEND_PID_FILE}"; then
    echo "backend already running (pid=$(cat "${BACKEND_PID_FILE}"))"
    return 0
  fi

  build_backend
  if [[ ! -x "${BACKEND_BIN}" ]]; then
    echo "backend binary not found: ${BACKEND_BIN}" >&2
    exit 1
  fi

  echo "starting backend..."
  (
    cd "${ROOT_DIR}/backend"
    nohup "${BACKEND_BIN}" >>"${BACKEND_LOG_FILE}" 2>&1 &
    echo $! > "${BACKEND_PID_FILE}"
  )
  local backend_pid
  backend_pid="$(cat "${BACKEND_PID_FILE}")"
  sleep 1
  if ! kill -0 "${backend_pid}" 2>/dev/null; then
    echo "backend failed to stay running (pid=${backend_pid}). check log: ${BACKEND_LOG_FILE}" >&2
    tail -n 40 "${BACKEND_LOG_FILE}" >&2 || true
    rm -f "${BACKEND_PID_FILE}"
    exit 1
  fi
  echo "backend started (pid=${backend_pid}) log=${BACKEND_LOG_FILE}"
}

start_frontend() {
  if is_running "${FRONTEND_PID_FILE}"; then
    echo "frontend already running (pid=$(cat "${FRONTEND_PID_FILE}"))"
    return 0
  fi

  echo "starting frontend..."
  (
    cd "${ROOT_DIR}/frontend"
    nohup npm run dev -- --host 0.0.0.0 --port 3001 >>"${FRONTEND_LOG_FILE}" 2>&1 &
    echo $! > "${FRONTEND_PID_FILE}"
  )
  echo "frontend started (pid=$(cat "${FRONTEND_PID_FILE}")) log=${FRONTEND_LOG_FILE}"
}

status_all() {
  if is_running "${BACKEND_PID_FILE}"; then
    echo "backend: running (pid=$(cat "${BACKEND_PID_FILE}"))"
  else
    echo "backend: stopped"
  fi

  if is_running "${FRONTEND_PID_FILE}"; then
    echo "frontend: running (pid=$(cat "${FRONTEND_PID_FILE}"))"
  else
    echo "frontend: stopped"
  fi

  local mysql_state sat_count
  if ensure_docker; then
    mysql_state="$(docker inspect -f '{{.State.Status}}' "${MYSQL_CONTAINER}" 2>/dev/null || echo stopped)"
    echo "mysql(${MYSQL_CONTAINER}) via docker: ${mysql_state}"
  else
    echo "mysql(${MYSQL_CONTAINER}) via docker: unavailable"
  fi

  if ensure_podman; then
    sat_count="$(podman ps -a --filter label=sfc.satellite=true --format '{{.ID}}' 2>/dev/null | wc -l | tr -d ' ')"
    echo "satellite pods via podman: ${sat_count}"
  else
    echo "satellite pods via podman: unavailable"
  fi
}

start_all() {
  start_infra
  start_backend
  start_frontend
  status_all
}

stop_all() {
  stop_one "frontend" "${FRONTEND_PID_FILE}"
  stop_one "backend" "${BACKEND_PID_FILE}"
  stop_infra
  status_all
}

case "${1:-start}" in
  start)
    start_all
    ;;
  stop)
    stop_all
    ;;
  restart)
    stop_all
    start_all
    ;;
  status)
    status_all
    ;;
  infra-up)
    start_infra
    status_all
    ;;
  infra-down)
    stop_infra
    status_all
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status|infra-up|infra-down}" >&2
    exit 1
    ;;
esac
