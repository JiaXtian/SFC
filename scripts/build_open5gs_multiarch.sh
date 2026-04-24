#!/usr/bin/env bash
set -euo pipefail

# Build and optionally push a multi-arch Open5GS satellite image.
#
# Usage:
#   IMAGE_REPO=ghcr.io/<org>/sfc-open5gs-satellite IMAGE_TAG=v0.1.0 ./scripts/build_open5gs_multiarch.sh
#
# Optional env:
#   PLATFORMS=linux/amd64,linux/arm64
#   DOCKERFILE=docker/open5gs-satellite.Dockerfile
#   BUILD_CONTEXT=.
#   PUSH=1            # 1: push manifest list, 0: local load (single platform only)
#   BUILDER=sfc-multiarch

IMAGE_REPO="${IMAGE_REPO:-ghcr.io/example/sfc-open5gs-satellite}"
IMAGE_TAG="${IMAGE_TAG:-latest}"
PLATFORMS="${PLATFORMS:-linux/amd64,linux/arm64}"
DOCKERFILE="${DOCKERFILE:-docker/open5gs-satellite.Dockerfile}"
BUILD_CONTEXT="${BUILD_CONTEXT:-.}"
PUSH="${PUSH:-1}"
BUILDER="${BUILDER:-sfc-multiarch}"

if ! command -v docker >/dev/null 2>&1; then
  echo "[ERROR] docker not found"
  exit 1
fi

if ! docker buildx version >/dev/null 2>&1; then
  echo "[ERROR] docker buildx not available"
  exit 1
fi

if [[ ! -f "$DOCKERFILE" ]]; then
  echo "[ERROR] Dockerfile not found: $DOCKERFILE"
  exit 1
fi

if ! docker buildx inspect "$BUILDER" >/dev/null 2>&1; then
  docker buildx create --name "$BUILDER" --use >/dev/null
else
  docker buildx use "$BUILDER" >/dev/null
fi

docker buildx inspect --bootstrap >/dev/null

IMAGE_REF="${IMAGE_REPO}:${IMAGE_TAG}"

echo "[INFO] image: $IMAGE_REF"
echo "[INFO] platforms: $PLATFORMS"

action_flag="--push"
if [[ "$PUSH" != "1" ]]; then
  if [[ "$PLATFORMS" == *","* ]]; then
    echo "[WARN] PUSH=0 with multiple platforms is not supported by --load; using first platform only"
    PLATFORMS="${PLATFORMS%%,*}"
  fi
  action_flag="--load"
fi

docker buildx build \
  --platform "$PLATFORMS" \
  -f "$DOCKERFILE" \
  -t "$IMAGE_REF" \
  $action_flag \
  "$BUILD_CONTEXT"

echo "[OK] build completed: $IMAGE_REF"
if [[ "$PUSH" == "1" ]]; then
  echo "[NEXT] set runtime image envs:"
  echo "       export SFC_SATELLITE_IMAGE_ARM64=${IMAGE_REF}"
  echo "       export SFC_SATELLITE_IMAGE_AMD64=${IMAGE_REF}"
fi
