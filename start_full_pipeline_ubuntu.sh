#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

SKIP_DATA=${SKIP_DATA:-0}
SKIP_TRAIN=${SKIP_TRAIN:-0}
SKIP_EXPORT=${SKIP_EXPORT:-0}
SKIP_BACKEND_BUILD=${SKIP_BACKEND_BUILD:-0}
SKIP_FRONTEND_INSTALL=${SKIP_FRONTEND_INSTALL:-0}

EPOCHS=${EPOCHS:-40}
TRAIN_RATIO=${TRAIN_RATIO:-0.85}
DEVICE=${DEVICE:-auto}

DYNAMIC_SCALE_PLAN=${DYNAMIC_SCALE_PLAN:-"800:2,1600:2,2500:3,4000:3,4800:2"}
DYNAMIC_DURATION_SEC=${DYNAMIC_DURATION_SEC:-180}
DYNAMIC_STEP_SEC=${DYNAMIC_STEP_SEC:-5}
DYNAMIC_REQ_PER_STEP=${DYNAMIC_REQ_PER_STEP:-64}

BACKEND_PORT=${BACKEND_PORT:-8080}
FRONTEND_PORT=${FRONTEND_PORT:-5173}

if [[ ! -d venv ]]; then
  python3 -m venv venv
fi
source venv/bin/activate

venv/bin/pip install -r train/requirements.txt

if [[ "${SKIP_DATA}" -eq 0 ]]; then
  venv/bin/python train/ground_training/data_generation/generate_dynamic_multiscale_data.py \
    --train-scale-plan "${DYNAMIC_SCALE_PLAN}" \
    --duration-sec "${DYNAMIC_DURATION_SEC}" \
    --step-sec "${DYNAMIC_STEP_SEC}" \
    --base-requests-per-step "${DYNAMIC_REQ_PER_STEP}" \
    --topology-dir "${ROOT_DIR}/train/data/train/dynamic/topologies" \
    --request-dir "${ROOT_DIR}/train/data/train/dynamic/requests" \
    --summary-file "${ROOT_DIR}/logs/dynamic_multiscale_generation_summary.json"
fi

if [[ "${SKIP_TRAIN}" -eq 0 ]]; then
  venv/bin/python -m train.ground_training.train_dynamic \
    --epochs "${EPOCHS}" \
    --device "${DEVICE}" \
    --train_ratio "${TRAIN_RATIO}" \
    --auto_expand_multiscale_data \
    --dynamic_scale_plan "${DYNAMIC_SCALE_PLAN}" \
    --dynamic_duration_sec "${DYNAMIC_DURATION_SEC}" \
    --dynamic_step_sec "${DYNAMIC_STEP_SEC}" \
    --dynamic_requests_per_step "${DYNAMIC_REQ_PER_STEP}" \
    --dynamic_topology_dir "${ROOT_DIR}/train/data/train/dynamic/topologies" \
    --dynamic_request_dir "${ROOT_DIR}/train/data/train/dynamic/requests"
fi

if [[ "${SKIP_EXPORT}" -eq 0 ]]; then
  venv/bin/python -m train.ground_training.models.model_export \
    --gnn-checkpoint models/checkpoints/gnn_dynamic_best.pth \
    --actor-checkpoint models/checkpoints/model_dynamic_best.pth \
    --output-dir models/exported \
    --context-dim 48
fi

if [[ "${SKIP_BACKEND_BUILD}" -eq 0 ]]; then
  if [[ -z "${ONNXRUNTIME_DIR:-}" ]]; then
    if [[ -d /usr/local/onnxruntime ]]; then
      export ONNXRUNTIME_DIR=/usr/local/onnxruntime
    elif [[ -d /usr/lib/onnxruntime ]]; then
      export ONNXRUNTIME_DIR=/usr/lib/onnxruntime
    fi
  fi
  mkdir -p backend/build
  pushd backend/build >/dev/null
  cmake -DCMAKE_BUILD_TYPE=Release ..
  cmake --build . -j"$(nproc)"
  popd >/dev/null
fi

if [[ "${SKIP_FRONTEND_INSTALL}" -eq 0 ]]; then
  pushd frontend >/dev/null
  npm install
  popd >/dev/null
fi

mkdir -p logs
pushd backend >/dev/null
./build/sfc_server > "${ROOT_DIR}/logs/backend_runtime.log" 2>&1 &
BACKEND_PID=$!
popd >/dev/null

cleanup() {
  if [[ -n "${BACKEND_PID:-}" ]] && kill -0 "${BACKEND_PID}" >/dev/null 2>&1; then
    kill "${BACKEND_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

echo "Backend PID: ${BACKEND_PID}, port=${BACKEND_PORT}"
echo "Frontend starting on port ${FRONTEND_PORT} ..."
pushd frontend >/dev/null
npm run dev -- --host 0.0.0.0 --port "${FRONTEND_PORT}"
popd >/dev/null
