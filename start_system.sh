#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

check_cmd() {
  local name="$1"
  if ! command -v "${name}" >/dev/null 2>&1; then
    echo "missing command: ${name}" >&2
    return 1
  fi
  return 0
}

echo "========================================"
echo " SFC Full Stack Start"
echo "========================================"

check_cmd cmake
check_cmd npm
check_cmd curl
check_cmd docker
if ! command -v podman >/dev/null 2>&1; then
  echo "warning: podman not found, satellite pod simulation will fall back to runtime error until podman is installed." >&2
fi

echo "[1/2] Starting infrastructure and services..."
./dev.sh start

echo ""
echo "[2/2] Start done."
echo "Frontend: http://127.0.0.1:3001"
echo "Backend : http://127.0.0.1:8080"
echo "Health  : http://127.0.0.1:8080/api/v1/health"
echo ""
echo "Use './stop_system.sh' to stop everything."
