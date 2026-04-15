#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

echo "========================================"
echo " SFC Full Stack Stop"
echo "========================================"

./dev.sh stop

echo "All services stopped."

