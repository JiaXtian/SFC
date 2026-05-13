#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "${ROOT_DIR}/.env.kylin" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.kylin"
fi

"${ROOT_DIR}/dev.sh" stop
