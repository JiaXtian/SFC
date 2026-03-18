#推理流程：./start.sh --skip-train   --skip-val-eval --skip-export --skip-build
set -euo pipefail

SKIP_DATA=0
SKIP_TRAIN=0
SKIP_VAL_EVAL=0
SKIP_EXPORT=0
SKIP_BUILD=0
SKIP_INFER=0

DEVICE="auto"
EPOCHS=160
MAX_REQUESTS_PER_FILE=24
MAX_DATA_FILES=24
WARMUP_EPOCHS=24
TIME_BUDGET_HOURS=6.0
MIN_EPOCHS=80
HEURISTIC_TOP_M=110

REL_CURR_START_EPOCH=1
REL_CURR_END_EPOCH=120
REL_CURR_MIN_SCALE=0.72
REL_CURR_STRICT_RATIO=0.78
REL_CURR_STRICT_RAMP_RATIO=0.24
SHARED_RESOURCES_PROB_MIN=0.18
SHARED_RESOURCES_PROB_MAX=0.42

TRAIN_TOPOLOGIES=18
TRAIN_GROUPS_PER_TOPOLOGY=7
TRAIN_REQUESTS_PER_GROUP=800
TRAIN_SCALES="2500,6000"
SCALE_DISTRIBUTION="6,5"
VAL_TOPOLOGIES=10
VAL_REQUESTS_PER_TOPOLOGY=500

VAL_EVAL_TOP_M=110
VAL_EVAL_MAX_REQUESTS=160

TOP_M=120
ONNXRUNTIME_DIR_ARG=""
TEST_TOPOLOGY_DIR="data/val/topologies"
TEST_REQUESTS_DIR="data/val/requests"
TEST_TOPOLOGY_FILE="data/val/topologies/topology_000.json"
TEST_REQUESTS_FILE="data/val/requests/requests_000.json"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<EOF
用法: ./start.sh [选项]

阶段跳过选项:
  --skip-data
  --skip-train
  --skip-val-eval
  --skip-export
  --skip-build
  --skip-infer

训练/数据参数:
  --device auto|cpu|cuda|mps
  --epochs N
  --max-requests-per-file N
  --max-data-files N
  --warmup-epochs N
  --time-budget-hours N
  --min-epochs N
  --heuristic-top-m N
  --train-topologies N
  --train-groups-per-topology N
  --train-requests-per-group N
  --train-scales CSV
  --scale-distribution CSV
  --val-topologies N
  --val-requests-per-topology N
  --val-eval-top-m N
  --val-eval-max-requests N
  --rel-curr-start-epoch N
  --rel-curr-end-epoch N
  --rel-curr-min-scale N
  --rel-curr-strict-ratio N
  --rel-curr-strict-ramp-ratio N
  --shared-resources-prob-min N
  --shared-resources-prob-max N

推理参数:
  --top-m N
  --onnxruntime-dir DIR
  --test-topology-dir DIR
  --test-requests-dir DIR
  --test-topology-file FILE
  --test-requests-file FILE
EOF
}

log() { echo -e "\n[$1] $2"; }
die() { echo "错误: $1" >&2; exit 1; }
have_cmd() { command -v "$1" >/dev/null 2>&1; }

jobs_for_make() {
  if have_cmd nproc; then nproc
  elif have_cmd sysctl; then sysctl -n hw.ncpu
  else echo 4
  fi
}

require_file() { [[ -f "$1" ]] || die "缺少文件: $1"; }
require_dir_nonempty() { [[ -d "$1" ]] || die "缺少目录: $1"; find "$1" -type f | head -n 1 >/dev/null || die "目录为空: $1"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-data) SKIP_DATA=1; shift ;;
    --skip-train) SKIP_TRAIN=1; shift ;;
    --skip-val-eval) SKIP_VAL_EVAL=1; shift ;;
    --skip-export) SKIP_EXPORT=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-infer) SKIP_INFER=1; shift ;;
    --device) DEVICE="$2"; shift 2 ;;
    --epochs) EPOCHS="$2"; shift 2 ;;
    --max-requests-per-file) MAX_REQUESTS_PER_FILE="$2"; shift 2 ;;
    --max-data-files) MAX_DATA_FILES="$2"; shift 2 ;;
    --warmup-epochs) WARMUP_EPOCHS="$2"; shift 2 ;;
    --time-budget-hours) TIME_BUDGET_HOURS="$2"; shift 2 ;;
    --min-epochs) MIN_EPOCHS="$2"; shift 2 ;;
    --heuristic-top-m) HEURISTIC_TOP_M="$2"; shift 2 ;;
    --rel-curr-start-epoch) REL_CURR_START_EPOCH="$2"; shift 2 ;;
    --rel-curr-end-epoch) REL_CURR_END_EPOCH="$2"; shift 2 ;;
    --rel-curr-min-scale) REL_CURR_MIN_SCALE="$2"; shift 2 ;;
    --rel-curr-strict-ratio) REL_CURR_STRICT_RATIO="$2"; shift 2 ;;
    --rel-curr-strict-ramp-ratio) REL_CURR_STRICT_RAMP_RATIO="$2"; shift 2 ;;
    --shared-resources-prob-min) SHARED_RESOURCES_PROB_MIN="$2"; shift 2 ;;
    --shared-resources-prob-max) SHARED_RESOURCES_PROB_MAX="$2"; shift 2 ;;
    --train-topologies) TRAIN_TOPOLOGIES="$2"; shift 2 ;;
    --train-groups-per-topology) TRAIN_GROUPS_PER_TOPOLOGY="$2"; shift 2 ;;
    --train-requests-per-group) TRAIN_REQUESTS_PER_GROUP="$2"; shift 2 ;;
    --train-scales) TRAIN_SCALES="$2"; shift 2 ;;
    --scale-distribution) SCALE_DISTRIBUTION="$2"; shift 2 ;;
    --val-topologies) VAL_TOPOLOGIES="$2"; shift 2 ;;
    --val-requests-per-topology) VAL_REQUESTS_PER_TOPOLOGY="$2"; shift 2 ;;
    --val-eval-top-m) VAL_EVAL_TOP_M="$2"; shift 2 ;;
    --val-eval-max-requests) VAL_EVAL_MAX_REQUESTS="$2"; shift 2 ;;
    --top-m) TOP_M="$2"; shift 2 ;;
    --onnxruntime-dir) ONNXRUNTIME_DIR_ARG="$2"; shift 2 ;;
    --test-topology-dir) TEST_TOPOLOGY_DIR="$2"; shift 2 ;;
    --test-requests-dir) TEST_REQUESTS_DIR="$2"; shift 2 ;;
    --test-topology-file) TEST_TOPOLOGY_FILE="$2"; shift 2 ;;
    --test-requests-file) TEST_REQUESTS_FILE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数: $1（使用 --help 查看帮助）" ;;
  esac
done

cd "$SCRIPT_DIR"
if [[ -f "${PROJECT_ROOT}/venv/bin/activate" ]]; then
  # shellcheck source=/dev/null
  source "${PROJECT_ROOT}/venv/bin/activate"
fi
have_cmd python || die "未找到 python 命令"

echo "=========================================="
echo "  SFC智能编排系统 训练与推理流程"
echo "=========================================="
echo "项目目录: $SCRIPT_DIR"
echo "跳过阶段: data=$SKIP_DATA train=$SKIP_TRAIN val_eval=$SKIP_VAL_EVAL export=$SKIP_EXPORT build=$SKIP_BUILD infer=$SKIP_INFER"

# 1) Data generation
if [[ "$SKIP_DATA" -eq 0 ]]; then
  log "1/6" "生成扩展训练集..."
  cd ground_training/data_generation
  python augment_data.py \
    --train_topologies "$TRAIN_TOPOLOGIES" \
    --train_groups_per_topology "$TRAIN_GROUPS_PER_TOPOLOGY" \
    --train_requests_per_group "$TRAIN_REQUESTS_PER_GROUP" \
    --train_scales "$TRAIN_SCALES" \
    --scale_distribution "$SCALE_DISTRIBUTION" \
    --val_topologies "$VAL_TOPOLOGIES" \
    --val_requests_per_topology "$VAL_REQUESTS_PER_TOPOLOGY"
  cd "$SCRIPT_DIR"
else
  log "1/6" "跳过数据生成"
fi

# 2) Train once (long run)
if [[ "$SKIP_TRAIN" -eq 0 ]]; then
  require_dir_nonempty "data/train/topologies"
  require_dir_nonempty "data/train/requests"
  log "2/6" "执行单次长训练..."
  python -m ground_training.train \
    --device "$DEVICE" \
    --epochs "$EPOCHS" \
    --max_requests_per_file "$MAX_REQUESTS_PER_FILE" \
    --max_data_files "$MAX_DATA_FILES" \
    --warmup_epochs "$WARMUP_EPOCHS" \
    --time_budget_hours "$TIME_BUDGET_HOURS" \
    --min_epochs "$MIN_EPOCHS" \
    --heuristic_top_m "$HEURISTIC_TOP_M" \
    --rel_curr_start_epoch "$REL_CURR_START_EPOCH" \
    --rel_curr_end_epoch "$REL_CURR_END_EPOCH" \
    --rel_curr_min_scale "$REL_CURR_MIN_SCALE" \
    --rel_curr_strict_ratio "$REL_CURR_STRICT_RATIO" \
    --rel_curr_strict_ramp_ratio "$REL_CURR_STRICT_RAMP_RATIO" \
    --shared_resources_prob_min "$SHARED_RESOURCES_PROB_MIN" \
    --shared_resources_prob_max "$SHARED_RESOURCES_PROB_MAX"
else
  log "2/6" "跳过训练"
fi

# 3) Validation evaluation
if [[ "$SKIP_VAL_EVAL" -eq 0 ]]; then
  require_file "models/checkpoints/gnn_best.pth"
  require_file "models/checkpoints/model_best.pth"
  log "3/6" "验证集评估..."
  python -m ground_training.evaluate \
    --model_checkpoint "models/checkpoints/model_best.pth" \
    --gnn_checkpoint "models/checkpoints/gnn_best.pth" \
    --val_topology_dir "data/val/topologies" \
    --val_requests_dir "data/val/requests" \
    --top_m "$VAL_EVAL_TOP_M" \
    --max_requests_per_file "$VAL_EVAL_MAX_REQUESTS" \
    --output_json "logs/val_eval.json"
else
  log "3/6" "跳过验证评估"
fi

# 4) Export ONNX
if [[ "$SKIP_EXPORT" -eq 0 ]]; then
  require_file "models/checkpoints/gnn_best.pth"
  require_file "models/checkpoints/model_best.pth"
  log "4/6" "导出ONNX模型..."
  python -m ground_training.models.model_export
else
  log "4/6" "跳过ONNX导出"
fi

# 5) Build C++ inference
if [[ "$SKIP_BUILD" -eq 0 ]]; then
  have_cmd cmake || die "未找到 cmake，请先安装"
  log "5/6" "编译星上推理引擎..."
  cd onboard_inference
  mkdir -p build
  cd build
  if [[ -f "CMakeCache.txt" ]]; then
    current_src_dir="$(cd .. && pwd)"
    cached_src_dir="$(grep '^CMAKE_HOME_DIRECTORY:INTERNAL=' CMakeCache.txt | cut -d= -f2- || true)"
    if [[ -n "$cached_src_dir" && "$cached_src_dir" != "$current_src_dir" ]]; then
      rm -f CMakeCache.txt
      rm -rf CMakeFiles
    fi
  fi
  if [[ -n "$ONNXRUNTIME_DIR_ARG" ]]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_DIR="$ONNXRUNTIME_DIR_ARG"
  else
    cmake .. -DCMAKE_BUILD_TYPE=Release
  fi
  make -j"$(jobs_for_make)"
  cd "$SCRIPT_DIR"
else
  log "5/6" "跳过推理引擎编译"
fi

# 6) Inference test 
#仅测试： ./start.sh   \--skip-data --skip-train  --skip-val-eval --skip-export
if [[ "$SKIP_INFER" -eq 0 ]]; then
  require_file "models/exported/gnn_encoder.onnx"
  require_file "models/exported/actor.onnx"
  require_file "onboard_inference/build/sfc_inference"
  mkdir -p results
  log "6/6" "执行星上推理测试..."
  if [[ -d "$TEST_TOPOLOGY_DIR" && -d "$TEST_REQUESTS_DIR" ]]; then
    ./onboard_inference/build/sfc_inference \
      --gnn_model models/exported/gnn_encoder.onnx \
      --actor_model models/exported/actor.onnx \
      --topology_dir "$TEST_TOPOLOGY_DIR" \
      --requests_dir "$TEST_REQUESTS_DIR" \
      --output results/final_results.json \
      --top_m "$TOP_M"
  else
    require_file "$TEST_TOPOLOGY_FILE"
    require_file "$TEST_REQUESTS_FILE"
    ./onboard_inference/build/sfc_inference \
      --gnn_model models/exported/gnn_encoder.onnx \
      --actor_model models/exported/actor.onnx \
      --topology "$TEST_TOPOLOGY_FILE" \
      --requests "$TEST_REQUESTS_FILE" \
      --output results/final_results.json \
      --top_m "$TOP_M"
  fi

  if [[ -f "results/final_results.json" ]]; then
    log "6/6" "生成推理分析图表..."
    python onboard_inference/plot_results.py \
      --input results/final_results.json \
      --output-dir results/plots
  fi
else
  log "6/6" "跳过星上推理测试"
fi

echo -e "\n=========================================="
echo "  训练指标: logs/training_metrics.json"
echo "  验证指标: logs/val_eval.json"
echo "  推理结果: results/final_results.json"
echo "  推理图表: results/plots"
echo "=========================================="
