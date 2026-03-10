#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"
CLEAN_BUILD=0

print_usage() {
  cat <<USAGE
Usage: ./build_ubuntu.sh [options]

Options:
  --debug        Build with Debug type
  --release      Build with Release type (default)
  --clean        Remove existing build directory before configure
  -h, --help     Show this help message
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)
      BUILD_TYPE="Debug"
      ;;
    --release)
      BUILD_TYPE="Release"
      ;;
    --clean)
      CLEAN_BUILD=1
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "Error: Unknown option '$1'"
      print_usage
      exit 1
      ;;
  esac
  shift
done

echo "========================================"
echo "  SFC Backend - Ubuntu Build Script"
echo "========================================"
echo ""

if [[ -f /etc/os-release ]]; then
  # shellcheck source=/dev/null
  . /etc/os-release
  DISTRO="${ID:-unknown}"
  if [[ "${DISTRO}" != "ubuntu" && "${DISTRO}" != "debian" ]]; then
    echo "Warning: detected distro '${DISTRO}'. This script targets Ubuntu/Debian."
  fi
fi

echo "[1/6] Checking toolchain..."
for cmd in cmake; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Error: '$cmd' not found."
    echo "Install with: sudo apt-get update && sudo apt-get install -y cmake"
    exit 1
  fi
done

if command -v g++ >/dev/null 2>&1; then
  CXX_COMPILER="$(command -v g++)"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_COMPILER="$(command -v clang++)"
else
  echo "Error: no C++ compiler found (g++/clang++)."
  echo "Install with: sudo apt-get update && sudo apt-get install -y build-essential"
  exit 1
fi

echo "  ✓ CMake: $(cmake --version | head -n1)"
echo "  ✓ C++ compiler: ${CXX_COMPILER}"

echo ""
echo "[2/6] Checking required dev packages..."
MISSING_PKGS=()
for pkg in libjsoncpp-dev nlohmann-json3-dev libspdlog-dev libssl-dev zlib1g-dev uuid-dev; do
  if ! dpkg -s "$pkg" >/dev/null 2>&1; then
    MISSING_PKGS+=("$pkg")
  fi
done

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
  echo "Warning: missing apt packages: ${MISSING_PKGS[*]}"
  echo "Install with:"
  echo "  sudo apt-get update && sudo apt-get install -y ${MISSING_PKGS[*]}"
else
  echo "  ✓ Common Ubuntu dependencies found"
fi

echo ""
echo "[3/6] Resolving ONNX Runtime..."
if [[ -z "${ONNXRUNTIME_DIR:-}" ]]; then
  CANDIDATES=(
    "${SCRIPT_DIR}/../onnxruntime-linux-x64-1.16.0"
    "${SCRIPT_DIR}/../onnxruntime"
    "/usr/local/onnxruntime"
    "/opt/onnxruntime"
  )
  for c in "${CANDIDATES[@]}"; do
    if [[ -f "${c}/lib/libonnxruntime.so" ]]; then
      ONNXRUNTIME_DIR="$c"
      break
    fi
  done
fi

if [[ -z "${ONNXRUNTIME_DIR:-}" || ! -f "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so" ]]; then
  echo "Error: ONNX Runtime not found."
  echo "Expected: <ONNXRUNTIME_DIR>/lib/libonnxruntime.so"
  echo ""
  echo "Example setup:"
  echo "  wget https://github.com/microsoft/onnxruntime/releases/download/v1.16.0/onnxruntime-linux-x64-1.16.0.tgz"
  echo "  tar -xzf onnxruntime-linux-x64-1.16.0.tgz"
  echo "  export ONNXRUNTIME_DIR=\$(pwd)/onnxruntime-linux-x64-1.16.0"
  exit 1
fi

export ONNXRUNTIME_DIR
echo "  ✓ ONNX Runtime: ${ONNXRUNTIME_DIR}"

echo ""
echo "[4/6] Preparing build directory..."
if [[ "${CLEAN_BUILD}" -eq 1 && -d "${BUILD_DIR}" ]]; then
  rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}"

echo ""
echo "[5/6] Configuring with CMake..."
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_PREFIX_PATH="/usr/local;/usr;${ONNXRUNTIME_DIR}"

echo ""
echo "[6/6] Building..."
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
else
  JOBS=4
fi
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

mkdir -p "${SCRIPT_DIR}/data" "${SCRIPT_DIR}/public"

echo ""
echo "========================================"
echo "  ✓ Build Complete (${BUILD_TYPE})"
echo "========================================"
echo "Executable: ${BUILD_DIR}/sfc_server"
echo ""
echo "Run with:"
echo "  cd ${BUILD_DIR}"
echo "  LD_LIBRARY_PATH=${ONNXRUNTIME_DIR}/lib:\$LD_LIBRARY_PATH ./sfc_server"
echo ""
