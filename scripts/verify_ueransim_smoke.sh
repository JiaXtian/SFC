#!/usr/bin/env bash
set -euo pipefail

API_BASE="${API_BASE:-http://127.0.0.1:8080/api/v1}"
SFC_NETWORK="${SFC_OPEN5GS_NETWORK:-sfc-open5gs-net}"
UERANSIM_IMAGE="${UERANSIM_IMAGE:-docker.io/free5gc/ueransim:latest}"
GNB_CONTAINER="${GNB_CONTAINER_NAME:-sfc-ueransim-gnb}"
UE_CONTAINER="${UE_CONTAINER_NAME:-sfc-ueransim-ue}"
VERIFY_TIMEOUT_SEC="${VERIFY_TIMEOUT_SEC:-120}"
KEEP_UERANSIM="${KEEP_UERANSIM:-0}"
DEPLOYMENT_ID="${DEPLOYMENT_ID:-}"
GNB_BIN_OVERRIDE="${GNB_BIN:-}"
UE_BIN_OVERRIDE="${UE_BIN:-}"

MCC="${UERANSIM_MCC:-999}"
MNC="${UERANSIM_MNC:-70}"
TAC="${UERANSIM_TAC:-1}"
SST="${UERANSIM_SST:-1}"
SD="${UERANSIM_SD:-}"
APN="${UERANSIM_APN:-internet}"
IMSI="${UERANSIM_IMSI:-999700000000001}"
KEY="${UERANSIM_KEY:-465B5CE8B199B49FAA5F0A2EE238A6BC}"
OPC="${UERANSIM_OPC:-E8ED289DEBA952E4283B54E88E6183CA}"

TMP_DIR=""

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "[ERROR] missing command: $cmd"
    exit 1
  fi
}

cleanup() {
  if [[ "$KEEP_UERANSIM" == "1" ]]; then
    echo "[INFO] KEEP_UERANSIM=1, containers/config kept for debugging"
    echo "       GNB=$GNB_CONTAINER UE=$UE_CONTAINER TMP_DIR=${TMP_DIR:-N/A}"
    return
  fi
  docker rm -f "$GNB_CONTAINER" "$UE_CONTAINER" >/dev/null 2>&1 || true
  if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
    rm -rf "$TMP_DIR"
  fi
}
trap cleanup EXIT

resolve_bin_path() {
  local container="$1"
  local override="$2"
  shift 2
  local candidates=("$@")
  local resolved=""

  resolve_candidate() {
    local candidate="$1"
    if docker exec "$container" sh -lc "test -x '$candidate'"; then
      printf '%s\n' "$candidate"
      return 0
    fi
    resolved="$(docker exec "$container" sh -lc "command -v '$candidate' 2>/dev/null || true" | tr -d '\r' | head -n 1)"
    if [[ -n "$resolved" ]]; then
      printf '%s\n' "$resolved"
      return 0
    fi
    return 1
  }

  if [[ -n "$override" ]]; then
    resolve_candidate "$override" && return 0
  fi

  local p
  for p in "${candidates[@]}"; do
    resolve_candidate "$p" && return 0
  done
  return 1
}

wait_for_log() {
  local container="$1"
  local logfile="$2"
  local pattern="$3"
  local timeout="$4"
  local deadline=$((SECONDS + timeout))
  while (( SECONDS < deadline )); do
    if docker exec "$container" sh -lc "test -f '$logfile' && grep -Eiq '$pattern' '$logfile'"; then
      return 0
    fi
    sleep 2
  done
  return 1
}

require_cmd docker
require_cmd curl
require_cmd jq

if ! docker network inspect "$SFC_NETWORK" >/dev/null 2>&1; then
  echo "[ERROR] docker network not found: $SFC_NETWORK"
  exit 1
fi

echo "[INFO] checking deployment readiness from $API_BASE/deployments"
all_deployments_json="$(curl -fsS "$API_BASE/deployments")"

if [[ -n "$DEPLOYMENT_ID" ]]; then
  ready_dep_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c --arg dep "$DEPLOYMENT_ID" \
        '[.[] | select(((.deployment_id // .backend_deployment_id // "") == $dep) and (.service_ready == true) and (.ready_for_ueransim == true))] | last // empty'
  )"
else
  ready_dep_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c '[.[] | select((.service_ready == true) and (.ready_for_ueransim == true))] | sort_by(.last_update_at // .deployed_at // "") | last // empty'
  )"
fi

if [[ -z "$ready_dep_json" ]]; then
  if [[ -n "$DEPLOYMENT_ID" ]]; then
    echo "[ERROR] deployment not ready for UERANSIM: $DEPLOYMENT_ID"
  else
    echo "[ERROR] no deployment with service_ready=true and ready_for_ueransim=true"
  fi
  exit 1
fi

ready_dep_id="$(printf '%s' "$ready_dep_json" | jq -r '.deployment_id // .backend_deployment_id // empty')"
echo "[INFO] selected deployment: $ready_dep_id"
deployed_nodes_json="$(printf '%s' "$ready_dep_json" | jq -c '.deployed_nodes // []')"

satellites_json="$(curl -fsS "$API_BASE/satellites?page=1&page_size=10000")"
amf_container="$(printf '%s' "$satellites_json" \
  | jq -r --argjson deployed_nodes "$deployed_nodes_json" '
      .items[]
      | .id as $id
      | select((($deployed_nodes | index($id)) != null))
      | select((.container_state // "") == "running")
      | select((((.running_core_nf_types // []) | map(ascii_downcase) | index("amf")) != null))
      | .container_name
    ' \
  | head -n 1)"

if [[ -z "$amf_container" ]]; then
  echo "[WARN] AMF container not found via API, trying docker process scan"
  dep_filter="$(printf '%s' "$ready_dep_id" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//; s/--+/-/g')"
  while IFS= read -r c; do
    if docker exec "$c" sh -lc 'pgrep -f "open5gs-amfd" >/dev/null 2>&1'; then
      amf_container="$c"
      break
    fi
  done < <(docker ps --format '{{.Names}}' | grep "^sfc-sat-${dep_filter}-" || true)
fi

if [[ -z "$amf_container" ]]; then
  echo "[ERROR] unable to locate AMF container"
  exit 1
fi

amf_ip="$(docker inspect "$amf_container" | jq -r --arg net "$SFC_NETWORK" '.[0].NetworkSettings.Networks[$net].IPAddress // empty')"
if [[ -z "$amf_ip" ]]; then
  amf_ip="$(docker inspect "$amf_container" | jq -r '.[0].NetworkSettings.Networks | to_entries[0].value.IPAddress // empty')"
fi
if [[ -z "$amf_ip" ]]; then
  echo "[ERROR] failed to resolve AMF container IP: $amf_container"
  exit 1
fi

echo "[INFO] AMF container=$amf_container, AMF IP=$amf_ip"
echo "[INFO] pulling UERANSIM image from Docker Hub: $UERANSIM_IMAGE"
docker pull "$UERANSIM_IMAGE" >/dev/null

docker rm -f "$GNB_CONTAINER" "$UE_CONTAINER" >/dev/null 2>&1 || true
TMP_DIR="$(mktemp -d -t sfc-ueransim.XXXXXX)"

docker run -d --name "$GNB_CONTAINER" --network "$SFC_NETWORK" -v "$TMP_DIR:/config" "$UERANSIM_IMAGE" sh -lc 'while true; do sleep 3600; done' >/dev/null
docker run -d --name "$UE_CONTAINER" --network "$SFC_NETWORK" -v "$TMP_DIR:/config" "$UERANSIM_IMAGE" sh -lc 'while true; do sleep 3600; done' >/dev/null

GNB_BIN_PATH="$(resolve_bin_path "$GNB_CONTAINER" "$GNB_BIN_OVERRIDE" /ueransim/nr-gnb /UERANSIM/build/nr-gnb nr-gnb || true)"
UE_BIN_PATH="$(resolve_bin_path "$UE_CONTAINER" "$UE_BIN_OVERRIDE" /ueransim/nr-ue /UERANSIM/build/nr-ue nr-ue || true)"
if [[ -z "$GNB_BIN_PATH" || -z "$UE_BIN_PATH" ]]; then
  echo "[ERROR] failed to locate UERANSIM binaries in image: $UERANSIM_IMAGE"
  echo "       gNB bin=${GNB_BIN_PATH:-NOT_FOUND}, UE bin=${UE_BIN_PATH:-NOT_FOUND}"
  exit 1
fi
echo "[INFO] UERANSIM binaries: gNB=$GNB_BIN_PATH UE=$UE_BIN_PATH"

gnb_ip="$(docker inspect "$GNB_CONTAINER" | jq -r --arg net "$SFC_NETWORK" '.[0].NetworkSettings.Networks[$net].IPAddress // empty')"
if [[ -z "$gnb_ip" ]]; then
  gnb_ip="$(docker inspect "$GNB_CONTAINER" | jq -r '.[0].NetworkSettings.Networks | to_entries[0].value.IPAddress // empty')"
fi
if [[ -z "$gnb_ip" ]]; then
  echo "[ERROR] failed to resolve gNB container IP"
  exit 1
fi

cat > "$TMP_DIR/gnb.yaml" <<YAML
gnbId: 1
mcc: '${MCC}'
mnc: '${MNC}'
nci: '0x000000010'
idLength: 32
tac: ${TAC}
ignoreStreamIds: true
linkIp: ${gnb_ip}
ngapIp: ${gnb_ip}
gtpIp: ${gnb_ip}
amfConfigs:
  - address: ${amf_ip}
    port: 38412
slices:
  - sst: ${SST}
YAML
if [[ -n "$SD" ]]; then
  echo "    sd: ${SD}" >> "$TMP_DIR/gnb.yaml"
fi

cat > "$TMP_DIR/ue.yaml" <<YAML
supi: 'imsi-${IMSI}'
mcc: '${MCC}'
mnc: '${MNC}'
key: '${KEY}'
opType: 'OPC'
op: '${OPC}'
amf: '8000'
imei: '356938035643803'
imeiSv: '4370816125816151'
integrity:
  IA1: true
  IA2: true
  IA3: true
ciphering:
  EA1: true
  EA2: true
  EA3: true
integrityMaxRate:
  uplink: full
  downlink: full
uacAic:
  mps: false
  mcs: false
uacAcc:
  normalClass: 0
  class11: false
  class12: false
  class13: false
  class14: false
  class15: false
gnbSearchList:
  - ${gnb_ip}
sessions:
  - type: 'IPv4'
    apn: '${APN}'
YAML

docker exec -d "$GNB_CONTAINER" sh -lc "'$GNB_BIN_PATH' -c /config/gnb.yaml > /tmp/gnb.log 2>&1"

echo "[INFO] waiting gNB NG setup ..."
if ! wait_for_log "$GNB_CONTAINER" /tmp/gnb.log 'NG Setup procedure is successful|NG setup successful|NG Setup Response' 60; then
  echo "[ERROR] gNB failed to establish NG setup"
  docker exec "$GNB_CONTAINER" sh -lc 'tail -n 120 /tmp/gnb.log || true'
  exit 1
fi

echo "[INFO] gNB NG setup success, starting UE ..."
docker exec -d "$UE_CONTAINER" sh -lc "'$UE_BIN_PATH' -c /config/ue.yaml > /tmp/ue.log 2>&1"

UE_PATTERN='Registration complete|Initial Registration is successful|PDU Session establishment is successful|PDU Session Establishment Accept'
if ! wait_for_log "$UE_CONTAINER" /tmp/ue.log "$UE_PATTERN" "$VERIFY_TIMEOUT_SEC"; then
  echo "[ERROR] UE smoke verification failed"
  echo "[INFO] gNB log tail:"
  docker exec "$GNB_CONTAINER" sh -lc 'tail -n 120 /tmp/gnb.log || true'
  echo "[INFO] UE log tail:"
  docker exec "$UE_CONTAINER" sh -lc 'tail -n 120 /tmp/ue.log || true'
  exit 1
fi

echo "[OK] UERANSIM smoke verification passed"
echo "[INFO] deployment=$ready_dep_id amf_container=$amf_container amf_ip=$amf_ip gnb_ip=$gnb_ip"
echo "[INFO] gNB recent logs:"
docker exec "$GNB_CONTAINER" sh -lc 'tail -n 40 /tmp/gnb.log || true'
echo "[INFO] UE recent logs:"
docker exec "$UE_CONTAINER" sh -lc 'tail -n 40 /tmp/ue.log || true'
