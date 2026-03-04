#!/bin/bash
set -e

# Ensure Homebrew tools (cmake, etc.) are discoverable on macOS.
if [ -d "/opt/homebrew/bin" ]; then
    export PATH="/opt/homebrew/bin:$PATH"
fi

echo "========================================"
echo "  SFC Backend - Build Script"
echo "========================================"
echo ""

# 检查依赖
echo "[1/5] Checking dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "Error: cmake not found. Please install cmake."
    exit 1
fi

if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "Error: C++ compiler not found."
    exit 1
fi

echo "  ✓ CMake found"
echo "  ✓ C++ compiler found"

# 检查Drogon / spdlog (Homebrew on macOS)
if [ ! -d "/opt/homebrew/lib/cmake/Drogon" ] || [ ! -d "/opt/homebrew/lib/cmake/spdlog" ]; then
    echo ""
    echo "Error: Missing C++ dependencies (Drogon/spdlog)."
    echo "Install with:"
    echo "  brew install drogon spdlog nlohmann-json"
    echo ""
    exit 1
fi

# 检查ONNX Runtime
if [ -z "$ONNXRUNTIME_DIR" ]; then
    echo ""
    echo "Warning: ONNXRUNTIME_DIR not set."
    if [ -d "/opt/homebrew/opt/onnxruntime" ]; then
        echo "Trying default path: /opt/homebrew/opt/onnxruntime"
        export ONNXRUNTIME_DIR=/opt/homebrew/opt/onnxruntime
    else
        echo "Trying default path: /usr/local/onnxruntime"
        export ONNXRUNTIME_DIR=/usr/local/onnxruntime
    fi
fi

if [ ! -d "$ONNXRUNTIME_DIR" ]; then
    echo ""
    echo "Error: ONNX Runtime not found at $ONNXRUNTIME_DIR"
    echo ""
    echo "Please download ONNX Runtime:"
    echo "  wget https://github.com/microsoft/onnxruntime/releases/download/v1.16.0/onnxruntime-linux-x64-1.16.0.tgz"
    echo "  tar -xzf onnxruntime-linux-x64-1.16.0.tgz"
    echo "  export ONNXRUNTIME_DIR=$(pwd)/onnxruntime-linux-x64-1.16.0"
    echo ""
    exit 1
fi

echo "  ✓ ONNX Runtime found at $ONNXRUNTIME_DIR"

# 创建构建目录
echo ""
echo "[2/5] Creating build directory..."
mkdir -p build
cd build

# CMake配置
echo ""
echo "[3/5] Configuring with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="/opt/homebrew;${ONNXRUNTIME_DIR}"

# 编译
echo ""
echo "[4/5] Building..."
if command -v nproc &> /dev/null; then
    JOBS=$(nproc)
else
    JOBS=$(sysctl -n hw.ncpu)
fi
make -j"$JOBS"

# 创建必要目录
echo ""
echo "[5/5] Creating runtime directories..."
cd ..
mkdir -p data public

echo ""
echo "========================================"
echo "  ✓ Build Complete!"
echo "========================================"
echo ""
echo "Executable: build/sfc_server"
echo ""
echo "To run:"
echo "  cd build"
echo "  ./sfc_server"
echo ""
