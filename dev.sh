#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="${ROOT_DIR}/.run"
PID_DIR="${RUNTIME_DIR}/pids"
LOG_DIR="${RUNTIME_DIR}/logs"

BACKEND_BUILD_DIR="${ROOT_DIR}/backend/build"
BACKEND_BIN="${BACKEND_BUILD_DIR}/sfc_server"
BACKEND_PID_FILE="${PID_DIR}/backend.pid"
FRONTEND_MAIN_PID_FILE="${PID_DIR}/frontend-main.pid"
FRONTEND_CONTROL_PID_FILE="${PID_DIR}/frontend-control.pid"
BACKEND_LOG_FILE="${LOG_DIR}/backend.log"
FRONTEND_MAIN_LOG_FILE="${LOG_DIR}/frontend-main.log"
FRONTEND_CONTROL_LOG_FILE="${LOG_DIR}/frontend-control.log"

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
  local wait_loops="${3:-20}"
  if ! is_running "${pid_file}"; then
    rm -f "${pid_file}"
    echo "${name} not running"
    return 0
  fi

  local pid
  pid="$(cat "${pid_file}")"
  kill "${pid}" 2>/dev/null || true
  for _ in $(seq 1 "${wait_loops}"); do
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

ensure_mysql_ready() {
  local mysql_container="sfc-mysql"
  local mysql_network="sfc-net"
  local mysql_data_dir="${RUNTIME_DIR}/mysql"
  local start_output=""
  local start_rc=0

  create_mysql_container() {
    local data_dir="$1"
    mkdir -p "${data_dir}"
    docker network inspect "${mysql_network}" >/dev/null 2>&1 || docker network create "${mysql_network}" >/dev/null
    docker run -d \
      --name "${mysql_container}" \
      --network "${mysql_network}" \
      -e MYSQL_ROOT_PASSWORD=root123456 \
      -e MYSQL_DATABASE=sfc_runtime \
      -e MYSQL_USER=sfc \
      -e MYSQL_PASSWORD=sfc123456 \
      -v "${data_dir}:/var/lib/mysql" \
      mysql:8.0 \
      --character-set-server=utf8mb4 \
      --collation-server=utf8mb4_unicode_ci >/dev/null
  }

  if ! command -v docker >/dev/null 2>&1; then
    echo "docker command not found, cannot start backend without MySQL" >&2
    exit 1
  fi

  if docker ps -a --format '{{.Names}}' | grep -q "^${mysql_container}$"; then
    if ! docker ps --format '{{.Names}}' | grep -q "^${mysql_container}$"; then
      echo "starting MySQL container (${mysql_container})..."
      start_output="$(docker start "${mysql_container}" 2>&1)" || start_rc=$?
      if [[ ${start_rc} -ne 0 ]]; then
        echo "MySQL start failed, recreating container: ${start_output}"
        local existing_data_dir
        existing_data_dir="$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/var/lib/mysql"}}{{.Source}}{{end}}{{end}}' "${mysql_container}" 2>/dev/null || true)"
        docker rm "${mysql_container}" >/dev/null 2>&1 || true
        if [[ -n "${existing_data_dir}" ]]; then
          mysql_data_dir="${existing_data_dir}"
        fi
        create_mysql_container "${mysql_data_dir}"
      fi
    fi
  else
    echo "MySQL container not found, creating ${mysql_container}..."
    create_mysql_container "${mysql_data_dir}"
  fi

  echo "waiting MySQL ready..."
  for _ in {1..60}; do
    if docker exec "${mysql_container}" mysql -usfc -psfc123456 -e "SELECT 1;" >/dev/null 2>&1; then
      echo "MySQL is ready"
      return 0
    fi
    sleep 1
  done

  echo "MySQL is not ready after timeout" >&2
  exit 1
}

cleanup_stale_backend_listener() {
  local listening_pids
  listening_pids="$(lsof -tiTCP:8080 -sTCP:LISTEN 2>/dev/null || true)"
  if [[ -z "${listening_pids}" ]]; then
    return 0
  fi

  while IFS= read -r pid; do
    [[ -z "${pid}" ]] && continue
    local cmdline
    cmdline="$(ps -p "${pid}" -o command= 2>/dev/null || true)"
    if [[ "${cmdline}" == *"sfc_server"* ]]; then
      echo "stopping stale backend listener on port 8080 (pid=${pid})..."
      kill "${pid}" 2>/dev/null || true
      for _ in {1..20}; do
        if ! kill -0 "${pid}" 2>/dev/null; then
          break
        fi
        sleep 0.2
      done
      if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
      fi
    else
      echo "port 8080 is occupied by a non-backend process (pid=${pid}): ${cmdline}" >&2
      return 1
    fi
  done <<< "${listening_pids}"

  return 0
}

start_backend() {
  cleanup_stale_backend_listener

  if is_running "${BACKEND_PID_FILE}"; then
    echo "backend already running (pid=$(cat "${BACKEND_PID_FILE}"))"
    return 0
  fi

  ensure_mysql_ready
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

start_frontend_main() {
  if is_running "${FRONTEND_MAIN_PID_FILE}"; then
    echo "frontend-main already running (pid=$(cat "${FRONTEND_MAIN_PID_FILE}"))"
    return 0
  fi

  echo "starting frontend-main (port 3001)..."
  (
    cd "${ROOT_DIR}/frontend"
    nohup npm run dev:main -- --host 0.0.0.0 --port 3001 >>"${FRONTEND_MAIN_LOG_FILE}" 2>&1 &
    echo $! > "${FRONTEND_MAIN_PID_FILE}"
  )
  echo "frontend-main started (pid=$(cat "${FRONTEND_MAIN_PID_FILE}")) log=${FRONTEND_MAIN_LOG_FILE}"
}

start_frontend_control() {
  if is_running "${FRONTEND_CONTROL_PID_FILE}"; then
    echo "frontend-control already running (pid=$(cat "${FRONTEND_CONTROL_PID_FILE}"))"
    return 0
  fi

  echo "starting frontend-control (port 3002)..."
  (
    cd "${ROOT_DIR}/frontend"
    nohup npm run dev:control -- --host 0.0.0.0 --port 3002 >>"${FRONTEND_CONTROL_LOG_FILE}" 2>&1 &
    echo $! > "${FRONTEND_CONTROL_PID_FILE}"
  )
  echo "frontend-control started (pid=$(cat "${FRONTEND_CONTROL_PID_FILE}")) log=${FRONTEND_CONTROL_LOG_FILE}"
}

status_all() {
  if is_running "${BACKEND_PID_FILE}"; then
    echo "backend: running (pid=$(cat "${BACKEND_PID_FILE}"))"
  else
    echo "backend: stopped"
  fi

  if is_running "${FRONTEND_MAIN_PID_FILE}"; then
    echo "frontend-main: running (pid=$(cat "${FRONTEND_MAIN_PID_FILE}"))"
  else
    echo "frontend-main: stopped"
  fi

  if is_running "${FRONTEND_CONTROL_PID_FILE}"; then
    echo "frontend-control: running (pid=$(cat "${FRONTEND_CONTROL_PID_FILE}"))"
  else
    echo "frontend-control: stopped"
  fi
}

start_all() {
  start_backend
  start_frontend_main
  start_frontend_control
  status_all
}

stop_all() {
  stop_one "frontend-control" "${FRONTEND_CONTROL_PID_FILE}"
  stop_one "frontend-main" "${FRONTEND_MAIN_PID_FILE}"
  stop_one "backend" "${BACKEND_PID_FILE}" 150
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
