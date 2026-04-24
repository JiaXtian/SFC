#!/usr/bin/env bash
set -euo pipefail

API_BASE="${API_BASE:-http://127.0.0.1:8080/api/v1}"
SFC_NETWORK="${SFC_OPEN5GS_NETWORK:-sfc-open5gs-net}"
UERANSIM_IMAGE="${UERANSIM_IMAGE:-ghcr.io/herlesupreeth/docker_ueransim:latest}"
GNB_CONTAINER="${GNB_CONTAINER_NAME:-sfc-ueransim-gnb}"
UE_CONTAINER="${UE_CONTAINER_NAME:-sfc-ueransim-ue}"
VERIFY_TIMEOUT_SEC="${VERIFY_TIMEOUT_SEC:-120}"
KEEP_UERANSIM="${KEEP_UERANSIM:-0}"

MCC="${UERANSIM_MCC:-999}"
MNC="${UERANSIM_MNC:-70}"
TAC="${UERANSIM_TAC:-1}"
SST="${UERANSIM_SST:-1}"
SD="${UERANSIM_SD:-000001}"
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

require_cmd docker
require_cmd curl
require_cmd jq

if ! docker network inspect "$SFC_NETWORK" >/dev/null 2>&1; then
  echo "[ERROR] docker network not found: $SFC_NETWORK"
  exit 1
fi

echo "[INFO] checking deployment readiness from $API_BASE/deployments"
ready_dep_json="$(curl -fsS "$API_BASE/deployments" | jq -c '[.[] | select((.service_ready == true) and (.ready_for_ueransim == true))] | sort_by(.last_update_at // .deployed_at // "") | last // empty')"
if [[ -z "$ready_dep_json" ]]; then
  echo "[ERROR] no deployment with service_ready=true and ready_for_ueransim=true"
  exit 1
fi

ready_dep_id="$(printf '%s' "$ready_dep_json" | jq -r '.deployment_id // .backend_deployment_id // empty')"
echo "[INFO] selected deployment: $ready_dep_id"

amf_container="$(curl -fsS "$API_BASE/satellites?page=1&page_size=2000" \
  | jq -r '.items[] | select(((.running_core_nf_types // []) | map(ascii_downcase) | index("amf")) != null) | .container_name' \
  | head -n 1)"

if [[ -z "$amf_container" ]]; then
  echo "[WARN] AMF container not found via API, trying docker process scan"
  while IFS= read -r c; do
    if docker exec "$c" sh -lc 'pgrep -f "open5gs-amfd" >/dev/null 2>&1'; then
      amf_container="$c"
      break
    fi
  done < <(docker ps --format '{{.Names}}' | grep '^sfc-sat-' || true)
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

docker rm -f "$GNB_CONTAINER" "$UE_CONTAINER" >/dev/null 2>&1 || true
TMP_DIR="$(mktemp -d -t sfc-ueransim.XXXXXX)"

# Start helper containers first so we can resolve gNB IP for UE config.
docker run -d --name "$GNB_CONTAINER" --network "$SFC_NETWORK" -v "$TMP_DIR:/config" "$UERANSIM_IMAGE" sh -lc 'while true; do sleep 3600; done' >/dev/null
docker run -d --name "$UE_CONTAINER" --network "$SFC_NETWORK" -v "$TMP_DIR:/config" "$UERANSIM_IMAGE" sh -lc 'while true; do sleep 3600; done' >/dev/null

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
linkIp: ${gnb_ip}
ngapIp: ${gnb_ip}
gtpIp: ${gnb_ip}
amfConfigs:
  - address: ${amf_ip}
    port: 38412
slices:
  - sst: ${SST}
    sd: ${SD}
YAML

cat > "$TMP_DIR/ue.yaml" <<YAML
supi: 'imsi-${IMSI}'
mcc: '${MCC}'
mnc: '${MNC}'
key: '${KEY}'
opType: 'OPC'
opValue: '${OPC}'
amf: '8000'
imei: '356938035643803'
imeiSv: '4370816125816151'
gnbSearchList:
  - ${gnb_ip}
sessions:
  - type: 'IPv4'
    apn: '${APN}'
    slice:
      sst: ${SST}
      sd: '${SD}'
configuredNssai:
  - sst: ${SST}
    sd: '${SD}'
defaultNssai:
  - sst: ${SST}
    sd: '${SD}'
YAML

docker exec -d "$GNB_CONTAINER" sh -lc 'nr-gnb -c /config/gnb.yaml > /tmp/gnb.log 2>&1'

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

echo "[INFO] waiting gNB NG setup ..."
if ! wait_for_log "$GNB_CONTAINER" /tmp/gnb.log 'NG Setup procedure is successful|NG setup successful|NG Setup Response' 60; then
  echo "[ERROR] gNB failed to establish NG setup"
  docker exec "$GNB_CONTAINER" sh -lc 'tail -n 120 /tmp/gnb.log || true'
  exit 1
fi

echo "[INFO] gNB NG setup success, starting UE ..."
docker exec -d "$UE_CONTAINER" sh -lc 'nr-ue -c /config/ue.yaml > /tmp/ue.log 2>&1'

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
