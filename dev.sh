#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="${ROOT_DIR}/.run"
PID_DIR="${RUNTIME_DIR}/pids"
LOG_DIR="${RUNTIME_DIR}/logs"

BACKEND_BUILD_DIR="${ROOT_DIR}/backend/build"
BACKEND_BIN="${BACKEND_BUILD_DIR}/sfc_server"
BACKEND_PID_FILE="${PID_DIR}/backend.pid"
FRONTEND_PID_FILE="${PID_DIR}/frontend.pid"
BACKEND_LOG_FILE="${LOG_DIR}/backend.log"
FRONTEND_LOG_FILE="${LOG_DIR}/frontend.log"

mkdir -p "${PID_DIR}" "${LOG_DIR}"

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
  echo "backend started (pid=$(cat "${BACKEND_PID_FILE}")) log=${BACKEND_LOG_FILE}"
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
}

start_all() {
  start_backend
  start_frontend
  status_all
}

stop_all() {
  stop_one "frontend" "${FRONTEND_PID_FILE}"
  stop_one "backend" "${BACKEND_PID_FILE}"
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
  *)
    echo "Usage: $0 {start|stop|restart|status}" >&2
    exit 1
    ;;
esac
