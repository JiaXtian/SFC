# Windows PowerShell training script for SFC project
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\start_windows.ps1
#
# Put this file in the same directory as the original start.sh, normally: project\SFC\train
# This simplified Windows version always runs all stages:
#   1) data generation
#   2) training
#   3) ONNX export
#   4) CMake build
#   5) inference benchmark and plot

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# =========================================================
# Configurable parameters
# Edit the values below directly when needed.
# =========================================================

# Training parameters
$Device = "auto"                 # auto | cpu | cuda | mps
$Epochs = 160
$MaxRequestsPerFile = 24
$MaxDataFiles = 24
$WarmupEpochs = 24
$TimeBudgetHours = 6.0
$MinEpochs = 80
$HeuristicTopM = 110

# Relative curriculum parameters
$RelCurrStartEpoch = 1
$RelCurrEndEpoch = 120
$RelCurrMinScale = 0.72
$RelCurrStrictRatio = 0.78
$RelCurrStrictRampRatio = 0.24
$SharedResourcesProbMin = 0.18
$SharedResourcesProbMax = 0.42

# Data generation parameters
$TrainTopologies = 18
$TrainGroupsPerTopology = 7
$TrainRequestsPerGroup = 800
$TrainScales = "2500,6000"
$ScaleDistribution = "6,5"
$ValTopologies = 10
$ValRequestsPerTopology = 500

# Inference / build parameters
$TopM = 120
$OnnxRuntimeDir = ""             # Example: C:\onnxruntime-win-x64-1.18.1, leave empty if CMake can find it
$TestTopologyDir = "data\val\topologies"
$TestRequestsDir = "data\val\requests"
$TestTopologyFile = "data\val\topologies\topology_000.json"
$TestRequestsFile = "data\val\requests\requests_000.json"

# =========================================================
# Helper functions
# =========================================================

function Write-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Step,
        [Parameter(Mandatory = $true)][string]$Message
    )
    Write-Host ""
    Write-Host "[$Step] $Message"
}

function Fail {
    param([Parameter(Mandatory = $true)][string]$Message)
    throw "错误: $Message"
}

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        Fail "未找到命令: $Name。请确认它已经安装，并且已经加入 PATH。"
    }
}

function Require-File {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "缺少文件: $Path"
    }
}

function Require-Dir-NonEmpty {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        Fail "缺少目录: $Path"
    }
    $firstFile = Get-ChildItem -LiteralPath $Path -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $firstFile) {
        Fail "目录为空: $Path"
    }
}

function Run-Command {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    Write-Host ""
    Write-Host "> $Command $($Arguments -join ' ')"
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "命令执行失败: $Command"
    }
}

function Resolve-BenchmarkExe {
    $candidates = @(
        "test\build\Release\model_benchmark.exe",
        "test\build\RelWithDebInfo\model_benchmark.exe",
        "test\build\Debug\model_benchmark.exe",
        "test\build\model_benchmark.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    Fail "未找到 model_benchmark.exe。请检查 CMake 编译是否成功。"
}

# =========================================================
# Path initialization
# =========================================================

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ScriptDir)) {
    $ScriptDir = (Get-Location).Path
}
$ProjectRoot = Split-Path -Parent $ScriptDir
$ModelExportDir = Join-Path $ProjectRoot "models\exported"
$ResultJson = Join-Path $ScriptDir "results\results.json"

Set-Location $ScriptDir

# Activate Windows venv if it exists
$ActivatePs1 = Join-Path $ProjectRoot "venv\Scripts\Activate.ps1"
if (Test-Path -LiteralPath $ActivatePs1 -PathType Leaf) {
    Write-Host "Activating venv: $ActivatePs1"
    . $ActivatePs1
}

Require-Command "python"
Require-Command "cmake"

Write-Host "=========================================="
Write-Host "  SFC智能编排系统 Windows 训练与推理流程"
Write-Host "=========================================="
Write-Host "脚本目录: $ScriptDir"
Write-Host "项目根目录: $ProjectRoot"
Write-Host "模型导出目录: $ModelExportDir"
Write-Host "设备: $Device"
Write-Host "训练轮数: $Epochs"
Write-Host "=========================================="

# =========================================================
# 1) Data generation
# =========================================================

Write-Step "1/5" "生成扩展训练集..."
Push-Location "training\data_generation"
try {
    Run-Command "python" @(
        "augment_data.py",
        "--train_topologies", "$TrainTopologies",
        "--train_groups_per_topology", "$TrainGroupsPerTopology",
        "--train_requests_per_group", "$TrainRequestsPerGroup",
        "--train_scales", "$TrainScales",
        "--scale_distribution", "$ScaleDistribution",
        "--val_topologies", "$ValTopologies",
        "--val_requests_per_topology", "$ValRequestsPerTopology"
    )
}
finally {
    Pop-Location
}

# =========================================================
# 2) Training
# =========================================================

Write-Step "2/5" "执行单次长训练..."
Require-Dir-NonEmpty "data\train\topologies"
Require-Dir-NonEmpty "data\train\requests"

Run-Command "python" @(
    "-m", "training.train",
    "--device", "$Device",
    "--epochs", "$Epochs",
    "--max_requests_per_file", "$MaxRequestsPerFile",
    "--max_data_files", "$MaxDataFiles",
    "--warmup_epochs", "$WarmupEpochs",
    "--time_budget_hours", "$TimeBudgetHours",
    "--min_epochs", "$MinEpochs",
    "--heuristic_top_m", "$HeuristicTopM",
    "--rel_curr_start_epoch", "$RelCurrStartEpoch",
    "--rel_curr_end_epoch", "$RelCurrEndEpoch",
    "--rel_curr_min_scale", "$RelCurrMinScale",
    "--rel_curr_strict_ratio", "$RelCurrStrictRatio",
    "--rel_curr_strict_ramp_ratio", "$RelCurrStrictRampRatio",
    "--shared_resources_prob_min", "$SharedResourcesProbMin",
    "--shared_resources_prob_max", "$SharedResourcesProbMax"
)

# =========================================================
# 3) Export ONNX
# =========================================================

Write-Step "3/5" "导出 ONNX 模型..."
Require-File "models\checkpoints\gnn_best.pth"
Require-File "models\checkpoints\model_best.pth"

Run-Command "python" @(
    "-m", "training.models.model_export",
    "--output-dir", "$ModelExportDir"
)

# =========================================================
# 4) Build C++ benchmark with CMake
# =========================================================

Write-Step "4/5" "编译 C++ 模型验证程序..."
Push-Location "test"
try {
    if (-not (Test-Path -LiteralPath "build" -PathType Container)) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }

    $cmakeConfigureArgs = @(
        "-S", ".",
        "-B", "build",
        "-DCMAKE_BUILD_TYPE=Release"
    )

    if (-not [string]::IsNullOrWhiteSpace($OnnxRuntimeDir)) {
        $cmakeConfigureArgs += "-DONNXRUNTIME_DIR=$OnnxRuntimeDir"
    }

    Run-Command "cmake" $cmakeConfigureArgs
    Run-Command "cmake" @("--build", "build", "--config", "Release", "--parallel")
}
finally {
    Pop-Location
}

# =========================================================
# 5) Inference benchmark test and plot
# =========================================================

Write-Step "5/5" "执行 C++ 模型有效性与速度测试..."
Require-File (Join-Path $ModelExportDir "gnn_encoder.onnx")
Require-File (Join-Path $ModelExportDir "actor.onnx")

$BenchmarkExe = Resolve-BenchmarkExe

if (-not (Test-Path -LiteralPath "results" -PathType Container)) {
    New-Item -ItemType Directory -Path "results" | Out-Null
}
Get-ChildItem -LiteralPath "results" -Filter "*.json" -File -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "  选用模型: $(Join-Path $ModelExportDir 'gnn_encoder.onnx')"
Write-Host "  选用模型: $(Join-Path $ModelExportDir 'actor.onnx')"

$inferArgs = @(
    "--gnn_model", "$(Join-Path $ModelExportDir 'gnn_encoder.onnx')",
    "--actor_model", "$(Join-Path $ModelExportDir 'actor.onnx')",
    "--output", "$ResultJson",
    "--top_m", "$TopM"
)

if ((Test-Path -LiteralPath $TestTopologyDir -PathType Container) -and (Test-Path -LiteralPath $TestRequestsDir -PathType Container)) {
    $inferArgs += @(
        "--topology_dir", "$TestTopologyDir",
        "--requests_dir", "$TestRequestsDir"
    )
}
else {
    Require-File $TestTopologyFile
    Require-File $TestRequestsFile
    $inferArgs += @(
        "--topology", "$TestTopologyFile",
        "--requests", "$TestRequestsFile"
    )
}

Run-Command $BenchmarkExe $inferArgs

if (Test-Path -LiteralPath $ResultJson -PathType Leaf) {
    Write-Step "5/5" "生成推理时延图..."
    Run-Command "python" @(
        "test\plot_results.py",
        "--input", "$ResultJson",
        "--output", "results\inference_latency_by_topology.png"
    )
}

Write-Host ""
Write-Host "=========================================="
Write-Host "  流程完成"
Write-Host "  训练指标: logs\training_metrics.json"
Write-Host "  模型导出: $ModelExportDir"
Write-Host "  推理引擎测试结果: $ResultJson"
Write-Host "  结果图表: results\inference_latency_by_topology.png"
Write-Host "=========================================="
