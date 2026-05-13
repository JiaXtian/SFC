# start.ps1
# Windows PowerShell version of start.sh
# Put this file in: SFC\train\start.ps1
#
# Examples:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1 --skip-train --skip-export --skip-build
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1 --device cpu --epochs 60

$ErrorActionPreference = "Stop"

# ============================================================
# Default parameters, same as the bash script
# ============================================================

$SKIP_DATA = 0
$SKIP_TRAIN = 0
$SKIP_EXPORT = 0
$SKIP_BUILD = 0
$SKIP_INFER = 0

$DEVICE = "auto"

$EPOCHS = 60
$MAX_REQUESTS_PER_FILE = 12
$MAX_DATA_FILES = 12
$WARMUP_EPOCHS = 5
$TIME_BUDGET_HOURS = 0.0
$MIN_EPOCHS = 0
$HEURISTIC_TOP_M = 64
$EVAL_DATA_FILES = 6
$EVAL_REQUESTS_PER_FILE = 8

$REL_CURR_START_EPOCH = 1
$REL_CURR_END_EPOCH = 32
$REL_CURR_MIN_SCALE = 0.72
$REL_CURR_STRICT_RATIO = 0.78
$REL_CURR_STRICT_RAMP_RATIO = 0.24
$SHARED_RESOURCES_PROB_MIN = 0.60
$SHARED_RESOURCES_PROB_MAX = 0.90
$CONTINUOUS_GROUP_LEN_MIN = 5
$CONTINUOUS_GROUP_LEN_MAX = 12
$EVAL_CONTINUOUS_GROUP_LEN = 12

$TRAIN_TOPOLOGIES = 12
$TRAIN_GROUPS_PER_TOPOLOGY = 2
$TRAIN_REQUESTS_PER_GROUP = 64
$TRAIN_SCALES = "300,800,2500,5000,6000"
$SCALE_DISTRIBUTION = "2,2,3,2,1"
$VAL_TOPOLOGIES = 6
$VAL_REQUESTS_PER_TOPOLOGY = 48

$TOP_M = 80
$ONNXRUNTIME_DIR_ARG = ""
$TEST_TOPOLOGY_DIR = "data\val\topologies"
$TEST_REQUESTS_DIR = "data\val\requests"
$TEST_TOPOLOGY_FILE = "data\val\topologies\topology_000.json"
$TEST_REQUESTS_FILE = "data\val\requests\requests_000.json"

# ============================================================
# Helper functions
# ============================================================

function Show-Usage {
    Write-Host @"
Usage: powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1 [options]

Stage skip options:
  --skip-data
  --skip-train
  --skip-export
  --skip-build
  --skip-infer

Training/data options:
  --device auto|cpu|cuda|mps
  --epochs N
  --max-requests-per-file N
  --max-data-files N
  --warmup-epochs N
  --time-budget-hours N
  --min-epochs N
  --heuristic-top-m N
  --eval-data-files N
  --eval-requests-per-file N
  --train-topologies N
  --train-groups-per-topology N
  --train-requests-per-group N
  --train-scales CSV
  --scale-distribution CSV
  --val-topologies N
  --val-requests-per-topology N
  --rel-curr-start-epoch N
  --rel-curr-end-epoch N
  --rel-curr-min-scale N
  --rel-curr-strict-ratio N
  --rel-curr-strict-ramp-ratio N
  --shared-resources-prob-min N
  --shared-resources-prob-max N
  --continuous-group-len-min N
  --continuous-group-len-max N
  --eval-continuous-group-len N

Inference/build options:
  --top-m N
  --onnxruntime-dir DIR
  --test-topology-dir DIR
  --test-requests-dir DIR
  --test-topology-file FILE
  --test-requests-file FILE
"@
}

function Log-Step {
    param(
        [string]$Step,
        [string]$Message
    )
    Write-Host ""
    Write-Host "[$Step] $Message"
}

function Die {
    param([string]$Message)
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Have-Command {
    param([string]$CommandName)
    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    return ($null -ne $cmd)
}

function Require-Command {
    param([string]$CommandName)
    if (-not (Have-Command $CommandName)) {
        Die "Command not found: $CommandName"
    }
}

function Require-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Die "Missing file: $Path"
    }
}

function Require-Dir-NonEmpty {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        Die "Missing directory: $Path"
    }

    $oneFile = Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $oneFile) {
        Die "Directory is empty: $Path"
    }
}

function Jobs-For-CMake {
    $n = [Environment]::ProcessorCount
    if ($n -lt 1) {
        return 4
    }
    return $n
}

function Resolve-BenchmarkExe {
    $candidates = @(
        "test\build\Release\model_benchmark.exe",
        "test\build\Debug\model_benchmark.exe",
        "test\build\RelWithDebInfo\model_benchmark.exe",
        "test\build\MinSizeRel\model_benchmark.exe",
        "test\build\model_benchmark.exe",
        "test\build\model_benchmark"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return $null
}

function Resolve-Checkpoint {
    param(
        [string]$BestPath,
        [string]$FinalPath
    )

    if (Test-Path -LiteralPath $BestPath -PathType Leaf) {
        return $BestPath
    }

    if (Test-Path -LiteralPath $FinalPath -PathType Leaf) {
        return $FinalPath
    }

    Die "Missing model file: $BestPath or $FinalPath"
}

function Need-Value {
    param(
        [string]$Option,
        [int]$Index,
        [object[]]$AllArgs
    )

    if (($Index + 1) -ge $AllArgs.Count) {
        Die "Missing value for option: $Option"
    }

    return [string]$AllArgs[$Index + 1]
}

# ============================================================
# Parse command line options, compatible with bash-style --xxx
# ============================================================

$i = 0
while ($i -lt $args.Count) {
    $arg = [string]$args[$i]

    switch ($arg) {
        "--skip-data" {
            $SKIP_DATA = 1
            $i += 1
        }
        "--skip-train" {
            $SKIP_TRAIN = 1
            $i += 1
        }
        "--skip-export" {
            $SKIP_EXPORT = 1
            $i += 1
        }
        "--skip-build" {
            $SKIP_BUILD = 1
            $i += 1
        }
        "--skip-infer" {
            $SKIP_INFER = 1
            $i += 1
        }
        "--device" {
            $DEVICE = Need-Value $arg $i $args
            $i += 2
        }
        "--epochs" {
            $EPOCHS = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--max-requests-per-file" {
            $MAX_REQUESTS_PER_FILE = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--max-data-files" {
            $MAX_DATA_FILES = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--warmup-epochs" {
            $WARMUP_EPOCHS = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--time-budget-hours" {
            $TIME_BUDGET_HOURS = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--min-epochs" {
            $MIN_EPOCHS = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--heuristic-top-m" {
            $HEURISTIC_TOP_M = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--eval-data-files" {
            $EVAL_DATA_FILES = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--eval-requests-per-file" {
            $EVAL_REQUESTS_PER_FILE = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--rel-curr-start-epoch" {
            $REL_CURR_START_EPOCH = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--rel-curr-end-epoch" {
            $REL_CURR_END_EPOCH = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--rel-curr-min-scale" {
            $REL_CURR_MIN_SCALE = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--rel-curr-strict-ratio" {
            $REL_CURR_STRICT_RATIO = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--rel-curr-strict-ramp-ratio" {
            $REL_CURR_STRICT_RAMP_RATIO = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--shared-resources-prob-min" {
            $SHARED_RESOURCES_PROB_MIN = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--shared-resources-prob-max" {
            $SHARED_RESOURCES_PROB_MAX = [double](Need-Value $arg $i $args)
            $i += 2
        }
        "--continuous-group-len-min" {
            $CONTINUOUS_GROUP_LEN_MIN = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--continuous-group-len-max" {
            $CONTINUOUS_GROUP_LEN_MAX = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--eval-continuous-group-len" {
            $EVAL_CONTINUOUS_GROUP_LEN = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--train-topologies" {
            $TRAIN_TOPOLOGIES = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--train-groups-per-topology" {
            $TRAIN_GROUPS_PER_TOPOLOGY = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--train-requests-per-group" {
            $TRAIN_REQUESTS_PER_GROUP = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--train-scales" {
            $TRAIN_SCALES = Need-Value $arg $i $args
            $i += 2
        }
        "--scale-distribution" {
            $SCALE_DISTRIBUTION = Need-Value $arg $i $args
            $i += 2
        }
        "--val-topologies" {
            $VAL_TOPOLOGIES = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--val-requests-per-topology" {
            $VAL_REQUESTS_PER_TOPOLOGY = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--top-m" {
            $TOP_M = [int](Need-Value $arg $i $args)
            $i += 2
        }
        "--onnxruntime-dir" {
            $ONNXRUNTIME_DIR_ARG = Need-Value $arg $i $args
            $i += 2
        }
        "--test-topology-dir" {
            $TEST_TOPOLOGY_DIR = Need-Value $arg $i $args
            $i += 2
        }
        "--test-requests-dir" {
            $TEST_REQUESTS_DIR = Need-Value $arg $i $args
            $i += 2
        }
        "--test-topology-file" {
            $TEST_TOPOLOGY_FILE = Need-Value $arg $i $args
            $i += 2
        }
        "--test-requests-file" {
            $TEST_REQUESTS_FILE = Need-Value $arg $i $args
            $i += 2
        }
        "-h" {
            Show-Usage
            exit 0
        }
        "--help" {
            Show-Usage
            exit 0
        }
        default {
            Die "Unknown option: $arg. Use --help for usage."
        }
    }
}

# ============================================================
# Directory setup
# ============================================================

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_ROOT = Split-Path -Parent $SCRIPT_DIR
$MODEL_EXPORT_DIR = Join-Path $PROJECT_ROOT "models\exported"
$MODEL_CHECKPOINT_DIR = Join-Path $PROJECT_ROOT "models\checkpoints"
$RESULT_JSON = Join-Path $SCRIPT_DIR "results\results.json"

Set-Location $SCRIPT_DIR

# Activate venv on Windows if it exists.
$venvActivate = Join-Path $PROJECT_ROOT "venv\Scripts\Activate.ps1"
if (Test-Path -LiteralPath $venvActivate -PathType Leaf) {
    . $venvActivate
}

Require-Command "python"

Write-Host "=========================================="
Write-Host "  open5gs constellation core network training and inference pipeline"
Write-Host "=========================================="
Write-Host "Project directory: $SCRIPT_DIR"
Write-Host "Skip stages: data=$SKIP_DATA train=$SKIP_TRAIN export=$SKIP_EXPORT build=$SKIP_BUILD infer=$SKIP_INFER"

# ============================================================
# 1) Data generation
# ============================================================

if ($SKIP_DATA -eq 0) {
    Log-Step "1/5" "Generating extended training dataset..."

    Set-Location (Join-Path $SCRIPT_DIR "training\data_generation")

    $dataArgs = @(
        "augment_data.py",
        "--train_topologies", "$TRAIN_TOPOLOGIES",
        "--train_groups_per_topology", "$TRAIN_GROUPS_PER_TOPOLOGY",
        "--train_requests_per_group", "$TRAIN_REQUESTS_PER_GROUP",
        "--train_scales", "$TRAIN_SCALES",
        "--scale_distribution", "$SCALE_DISTRIBUTION",
        "--val_topologies", "$VAL_TOPOLOGIES",
        "--val_requests_per_topology", "$VAL_REQUESTS_PER_TOPOLOGY"
    )

    & python @dataArgs
    if ($LASTEXITCODE -ne 0) {
        Die "Data generation failed."
    }

    Set-Location $SCRIPT_DIR
}
else {
    Log-Step "1/5" "Skip data generation"
}

# ============================================================
# 2) Train once
# ============================================================

if ($SKIP_TRAIN -eq 0) {
    Require-Dir-NonEmpty "data\train\topologies"
    Require-Dir-NonEmpty "data\train\requests"

    Log-Step "2/5" "Running one long training job..."

    $trainArgs = @(
        "-m", "training.train",
        "--device", "$DEVICE",
        "--epochs", "$EPOCHS",
        "--max_requests_per_file", "$MAX_REQUESTS_PER_FILE",
        "--max_data_files", "$MAX_DATA_FILES",
        "--warmup_epochs", "$WARMUP_EPOCHS",
        "--time_budget_hours", "$TIME_BUDGET_HOURS",
        "--min_epochs", "$MIN_EPOCHS",
        "--heuristic_top_m", "$HEURISTIC_TOP_M",
        "--eval_data_files", "$EVAL_DATA_FILES",
        "--eval_requests_per_file", "$EVAL_REQUESTS_PER_FILE",
        "--rel_curr_start_epoch", "$REL_CURR_START_EPOCH",
        "--rel_curr_end_epoch", "$REL_CURR_END_EPOCH",
        "--rel_curr_min_scale", "$REL_CURR_MIN_SCALE",
        "--rel_curr_strict_ratio", "$REL_CURR_STRICT_RATIO",
        "--rel_curr_strict_ramp_ratio", "$REL_CURR_STRICT_RAMP_RATIO",
        "--shared_resources_prob_min", "$SHARED_RESOURCES_PROB_MIN",
        "--shared_resources_prob_max", "$SHARED_RESOURCES_PROB_MAX",
        "--continuous_group_len_min", "$CONTINUOUS_GROUP_LEN_MIN",
        "--continuous_group_len_max", "$CONTINUOUS_GROUP_LEN_MAX",
        "--eval_continuous_group_len", "$EVAL_CONTINUOUS_GROUP_LEN"
    )

    & python @trainArgs
    if ($LASTEXITCODE -ne 0) {
        Die "Training failed."
    }
}
else {
    Log-Step "2/5" "Skip training"
}

# ============================================================
# 3) Export ONNX
# ============================================================

if ($SKIP_EXPORT -eq 0) {
    $gnnCheckpoint = Resolve-Checkpoint `
        (Join-Path $MODEL_CHECKPOINT_DIR "gnn_best.pth") `
        (Join-Path $MODEL_CHECKPOINT_DIR "gnn_final.pth")
    $actorCheckpoint = Resolve-Checkpoint `
        (Join-Path $MODEL_CHECKPOINT_DIR "model_best.pth") `
        (Join-Path $MODEL_CHECKPOINT_DIR "model_final.pth")

    Log-Step "3/5" "Exporting ONNX models..."
    Write-Host "  GNN checkpoint: $gnnCheckpoint"
    Write-Host "  Actor checkpoint: $actorCheckpoint"

    New-Item -ItemType Directory -Force -Path $MODEL_EXPORT_DIR | Out-Null

    $exportArgs = @(
        "-m", "training.models.model_export",
        "--gnn-checkpoint", "$gnnCheckpoint",
        "--actor-checkpoint", "$actorCheckpoint",
        "--output-dir", "$MODEL_EXPORT_DIR"
    )

    & python @exportArgs
    if ($LASTEXITCODE -ne 0) {
        Die "ONNX export failed."
    }
}
else {
    Log-Step "3/5" "Skip ONNX export"
}

# ============================================================
# 4) Build C++ benchmark
# ============================================================

if ($SKIP_BUILD -eq 0) {
    Require-Command "cmake"

    Log-Step "4/5" "Building C++ model benchmark..."

    Set-Location (Join-Path $SCRIPT_DIR "test")

    New-Item -ItemType Directory -Force -Path "build" | Out-Null
    Set-Location "build"

    if (Test-Path -LiteralPath "CMakeCache.txt" -PathType Leaf) {
        $currentSrcDir = (Resolve-Path "..").Path
        $cachedLine = Select-String -Path "CMakeCache.txt" -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=" -ErrorAction SilentlyContinue | Select-Object -First 1

        if ($null -ne $cachedLine) {
            $cachedSrcDir = ($cachedLine.Line -split "=", 2)[1]
            if (($cachedSrcDir -ne "") -and ($cachedSrcDir -ne $currentSrcDir)) {
                Remove-Item -LiteralPath "CMakeCache.txt" -Force -ErrorAction SilentlyContinue
                Remove-Item -LiteralPath "CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($ONNXRUNTIME_DIR_ARG -ne "") {
        & cmake .. -DCMAKE_BUILD_TYPE=Release "-DONNXRUNTIME_DIR=$ONNXRUNTIME_DIR_ARG"
    }
    else {
        & cmake .. -DCMAKE_BUILD_TYPE=Release
    }

    if ($LASTEXITCODE -ne 0) {
        Die "CMake configure failed."
    }

    $jobs = Jobs-For-CMake
    & cmake --build . --config Release --parallel $jobs

    if ($LASTEXITCODE -ne 0) {
        Die "CMake build failed."
    }

    Set-Location $SCRIPT_DIR
}
else {
    Log-Step "4/5" "Skip C++ model benchmark build"
}

# ============================================================
# 5) Inference benchmark test
# Bash example:
#   ./start.sh --skip-data --skip-train --skip-export
# PowerShell example:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1 --skip-data --skip-train --skip-export
# ============================================================

if ($SKIP_INFER -eq 0) {
    Require-File (Join-Path $MODEL_EXPORT_DIR "gnn_encoder.onnx")
    Require-File (Join-Path $MODEL_EXPORT_DIR "actor.onnx")

    $benchmarkExe = Resolve-BenchmarkExe
    if ($null -eq $benchmarkExe) {
        Die "Missing benchmark executable. Expected model_benchmark under test\build or test\build\Release."
    }

    New-Item -ItemType Directory -Force -Path "results" | Out-Null
    Get-ChildItem -LiteralPath "results" -Filter "*.json" -File -ErrorAction SilentlyContinue | Remove-Item -Force

    Log-Step "5/5" "Running C++ model validity and speed test..."

    $gnnModel = Join-Path $MODEL_EXPORT_DIR "gnn_encoder.onnx"
    $actorModel = Join-Path $MODEL_EXPORT_DIR "actor.onnx"

    Write-Host "  GNN model: $gnnModel"
    Write-Host "  Actor model: $actorModel"

    if ((Test-Path -LiteralPath $TEST_TOPOLOGY_DIR -PathType Container) -and
        (Test-Path -LiteralPath $TEST_REQUESTS_DIR -PathType Container)) {

        $inferArgs = @(
            "--gnn_model", "$gnnModel",
            "--actor_model", "$actorModel",
            "--topology_dir", "$TEST_TOPOLOGY_DIR",
            "--requests_dir", "$TEST_REQUESTS_DIR",
            "--output", "$RESULT_JSON",
            "--top_m", "$TOP_M"
        )
    }
    else {
        Require-File $TEST_TOPOLOGY_FILE
        Require-File $TEST_REQUESTS_FILE

        $inferArgs = @(
            "--gnn_model", "$gnnModel",
            "--actor_model", "$actorModel",
            "--topology", "$TEST_TOPOLOGY_FILE",
            "--requests", "$TEST_REQUESTS_FILE",
            "--output", "$RESULT_JSON",
            "--top_m", "$TOP_M"
        )
    }

    & $benchmarkExe @inferArgs

    if ($LASTEXITCODE -ne 0) {
        Die "Inference benchmark failed."
    }

    if (Test-Path -LiteralPath $RESULT_JSON -PathType Leaf) {
        Log-Step "5/5" "Generating inference latency figure..."

        $plotArgs = @(
            "test\plot_results.py",
            "--input", "$RESULT_JSON",
            "--output", "results\inference_latency_by_topology.png"
        )

        & python @plotArgs

        if ($LASTEXITCODE -ne 0) {
            Die "Plot generation failed."
        }
    }
}
else {
    Log-Step "5/5" "Skip C++ model test"
}

Write-Host ""
Write-Host "=========================================="
Write-Host "Training metrics: logs\training_metrics.json"
Write-Host "Model export: $MODEL_EXPORT_DIR"
Write-Host "Inference benchmark result: $RESULT_JSON"
Write-Host "Result figure: results\inference_latency_by_topology.png"
Write-Host "=========================================="
