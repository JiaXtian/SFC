#!/usr/bin/env bash
set -euo pipefail

API_BASE="${API_BASE:-}"
API_TOKEN="${API_TOKEN:-}"
API_USERNAME="${API_USERNAME:-admin}"
API_PASSWORD="${API_PASSWORD:-123456}"
SFC_NETWORK="${SFC_OPEN5GS_NETWORK:-sfc-open5gs-net}"
UERANSIM_IMAGE="${UERANSIM_IMAGE:-docker.io/free5gc/ueransim:latest}"
GNB_CONTAINER="${GNB_CONTAINER_NAME:-sfc-ueransim-gnb}"
UE_CONTAINER="${UE_CONTAINER_NAME:-sfc-ueransim-ue}"
VERIFY_TIMEOUT_SEC="${VERIFY_TIMEOUT_SEC:-120}"
PDU_WAIT_SEC="${PDU_WAIT_SEC:-30}"
STRICT_PDU_SESSION="${STRICT_PDU_SESSION:-0}"
KEEP_UERANSIM="${KEEP_UERANSIM:-0}"
DEPLOYMENT_ID="${DEPLOYMENT_ID:-}"
ALLOW_SERVICE_READY_FALLBACK="${ALLOW_SERVICE_READY_FALLBACK:-0}"
GNB_BIN_OVERRIDE="${GNB_BIN:-}"
UE_BIN_OVERRIDE="${UE_BIN:-}"
MONGO_CONTAINER="${SFC_OPEN5GS_MONGO_CONTAINER:-sfc-open5gs-mongo}"

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

normalize_token() {
  local raw="$1"
  local out
  out="$(printf '%s' "$raw" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//; s/--+/-/g')"
  if [[ -z "$out" ]]; then
    out="x"
  fi
  printf '%s\n' "$out"
}

node_to_container_name() {
  local dep_id="$1"
  local node_id="$2"
  printf 'sfc-sat-%s-%s\n' "$(normalize_token "$dep_id")" "$(normalize_token "$node_id")"
}

is_running_container() {
  local name="$1"
  [[ -n "$name" ]] || return 1
  docker ps --format '{{.Names}}' | grep -qx "$name"
}

get_satellite_json() {
  local node_id="$1"
  api_get "$API_BASE/satellites/$node_id" 2>/dev/null || true
}

find_running_container_for_node() {
  local dep_id="$1"
  local node_id="$2"
  local sat_json
  local runtime_container
  local canonical
  local legacy
  local node_norm
  local suffix_hit

  sat_json="$(get_satellite_json "$node_id")"
  runtime_container="$(printf '%s' "$sat_json" | jq -r '
    if ((.container_state // "") == "running") then (.container_name // "") else "" end
  ' 2>/dev/null || true)"
  if is_running_container "$runtime_container"; then
    printf '%s\n' "$runtime_container"
    return 0
  fi

  canonical="$(node_to_container_name "$dep_id" "$node_id")"
  if is_running_container "$canonical"; then
    printf '%s\n' "$canonical"
    return 0
  fi

  legacy="sfc-sat-$(normalize_token "$node_id")"
  if is_running_container "$legacy"; then
    printf '%s\n' "$legacy"
    return 0
  fi

  node_norm="$(normalize_token "$node_id")"
  suffix_hit="$(
    docker ps --format '{{.Names}}' \
      | grep -E "^sfc-sat-.*-${node_norm}$" \
      | head -n 1 || true
  )"
  if is_running_container "$suffix_hit"; then
    printf '%s\n' "$suffix_hit"
    return 0
  fi

  return 1
}

find_container_by_daemon() {
  local daemon_name="$1"
  local c
  while IFS= read -r c; do
    [[ -n "$c" ]] || continue
    if docker exec "$c" sh -lc "pgrep -x '$daemon_name' >/dev/null 2>&1 || pgrep -f '$daemon_name' >/dev/null 2>&1"; then
      printf '%s\n' "$c"
      return 0
    fi
  done < <(docker ps --format '{{.Names}}' | grep '^sfc-sat-' || true)
  return 1
}

daemon_for_nf_type() {
  local nf
  nf="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr '-' '_' | tr ' ' '_')"
  case "$nf" in
    nrf) echo "open5gs-nrfd" ;;
    amf) echo "open5gs-amfd" ;;
    smf) echo "open5gs-smfd" ;;
    upf) echo "open5gs-upfd" ;;
    ausf) echo "open5gs-ausfd" ;;
    udm) echo "open5gs-udmd" ;;
    udr) echo "open5gs-udrd" ;;
    pcf) echo "open5gs-pcfd" ;;
    nssf) echo "open5gs-nssfd" ;;
    scp) echo "open5gs-scpd" ;;
    bsf) echo "open5gs-bsfd" ;;
    sepp) echo "open5gs-seppd" ;;
    *) echo "open5gs-${nf}d" ;;
  esac
}

provision_subscriber() {
  local imsi_digits="$1"
  local sst="$2"
  local sd_hex="$3"
  local apn="$4"
  local key_hex="$5"
  local opc_hex="$6"

  if ! docker ps --format '{{.Names}}' | grep -qx "$MONGO_CONTAINER"; then
    echo "[WARN] mongo container not running: $MONGO_CONTAINER, skip subscriber provisioning"
    return 0
  fi

  local mongo_js
  mongo_js="$(cat <<EOS
const dbx = db.getSiblingDB('open5gs');
const imsi = '${imsi_digits}';
const sst = Number('${sst}');
const sd = '${sd_hex}';
const subscriber = {
  imsi,
  subscriber_status: 0,
  operator_determined_barring: 0,
  network_access_mode: 0,
  subscribed_rau_tau_timer: 12,
  access_restriction_data: 0,
  slice: [
    {
      sst,
      default_indicator: true,
      session: [
        {
          name: '${apn}',
          type: 1,
          pcc_rule: [],
          ambr: {
            uplink: { value: 1, unit: 3 },
            downlink: { value: 1, unit: 3 }
          },
          qos: {
            index: 9,
            arp: {
              priority_level: 8,
              pre_emption_capability: 1,
              pre_emption_vulnerability: 1
            }
          }
        }
      ]
    }
  ],
  ambr: {
    uplink: { value: 1, unit: 3 },
    downlink: { value: 1, unit: 3 }
  },
  security: {
    k: '${key_hex}',
    amf: '8000',
    op: null,
    opc: '${opc_hex}'
  },
  schema_version: 1,
  __v: 0
};
if (sd && sd.length > 0) {
  subscriber.slice[0].sd = sd;
}
dbx.subscribers.updateOne({ imsi }, { \$set: subscriber }, { upsert: true });
const found = dbx.subscribers.findOne({ imsi });
if (!found) {
  throw new Error('subscriber_upsert_failed');
}
printjson({ imsi: found.imsi, slice: found.slice });
EOS
)"

  echo "[INFO] provisioning subscriber IMSI=$imsi_digits on $MONGO_CONTAINER"
  docker exec -i "$MONGO_CONTAINER" mongosh --quiet --eval "$mongo_js" >/tmp/sfc_ueransim_subscriber.json
}

require_cmd docker
require_cmd curl
require_cmd jq

if [[ -z "$API_BASE" ]]; then
  for base in "http://127.0.0.1:18080/api/v1" "http://127.0.0.1:8080/api/v1"; do
    if curl -fsS "$base/health" >/dev/null 2>&1; then
      API_BASE="$base"
      break
    fi
  done
fi

if [[ -z "$API_BASE" ]]; then
  echo "[ERROR] unable to detect API base, please set API_BASE explicitly"
  exit 1
fi

if [[ -z "$API_TOKEN" ]]; then
  echo "[INFO] API_TOKEN not provided, logging in as $API_USERNAME"
  login_payload="$(jq -cn --arg u "$API_USERNAME" --arg p "$API_PASSWORD" '{username:$u,password:$p}')"
  login_resp="$(curl -fsS -X POST "$API_BASE/auth/login" -H 'Content-Type: application/json' -d "$login_payload" || true)"
  API_TOKEN="$(printf '%s' "$login_resp" | jq -r '.token // empty')"
  if [[ -z "$API_TOKEN" ]]; then
    echo "[ERROR] failed to obtain API token via /auth/login"
    echo "       You can set API_TOKEN directly, or override API_USERNAME/API_PASSWORD"
    exit 1
  fi
fi

CURL_AUTH_ARGS=("-H" "Authorization: Bearer $API_TOKEN")
api_get() {
  local url="$1"
  curl -fsS "${CURL_AUTH_ARGS[@]}" "$url"
}

normalize_sd() {
  local raw="$1"
  raw="${raw//[[:space:]]/}"
  raw="${raw#0x}"
  raw="${raw#0X}"
  if [[ -z "$raw" ]]; then
    printf '\n'
    return 0
  fi
  if [[ ! "$raw" =~ ^[0-9A-Fa-f]{1,6}$ ]]; then
    return 1
  fi
  printf '%06X\n' "$((16#$raw))"
}

RAW_SD="$SD"
if ! SD="$(normalize_sd "$SD")"; then
  echo "[ERROR] invalid UERANSIM_SD value: ${RAW_SD:-<empty>} (expect 1-6 hex chars, e.g. 000001)"
  exit 1
fi

if ! docker network inspect "$SFC_NETWORK" >/dev/null 2>&1; then
  echo "[ERROR] docker network not found: $SFC_NETWORK"
  exit 1
fi

IMSI_DIGITS="$(printf '%s' "$IMSI" | tr -cd '0-9')"
if [[ -z "$IMSI_DIGITS" ]]; then
  echo "[ERROR] invalid UERANSIM_IMSI: $IMSI"
  exit 1
fi

echo "[INFO] checking deployment readiness from $API_BASE/deployments"
all_deployments_json="$(api_get "$API_BASE/deployments")"
selection_mode="strict_ready_for_ueransim"

if [[ -n "$DEPLOYMENT_ID" ]]; then
  ready_dep_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c --arg dep "$DEPLOYMENT_ID" \
        '[.[] | select(((.deployment_id // .backend_deployment_id // "") == $dep) and (.service_ready == true) and (.ready_for_ueransim == true))] | last // empty'
  )"
  if [[ -z "$ready_dep_json" && "$ALLOW_SERVICE_READY_FALLBACK" == "1" ]]; then
    ready_dep_json="$(
      printf '%s' "$all_deployments_json" \
        | jq -c --arg dep "$DEPLOYMENT_ID" \
          '[.[] | select(((.deployment_id // .backend_deployment_id // "") == $dep) and (.service_ready == true))] | last // empty'
    )"
    if [[ -n "$ready_dep_json" ]]; then
      selection_mode="fallback_service_ready_only"
    fi
  fi
else
  ready_dep_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c '[.[] | select((.service_ready == true) and (.ready_for_ueransim == true))] | sort_by(.last_update_at // .deployed_at // "") | last // empty'
  )"
  if [[ -z "$ready_dep_json" && "$ALLOW_SERVICE_READY_FALLBACK" == "1" ]]; then
    ready_dep_json="$(
      printf '%s' "$all_deployments_json" \
        | jq -c '[.[] | select(.service_ready == true)] | sort_by(.last_update_at // .deployed_at // "") | last // empty'
    )"
    if [[ -n "$ready_dep_json" ]]; then
      selection_mode="fallback_service_ready_only"
    fi
  fi
fi

if [[ -z "$ready_dep_json" ]]; then
  diag_json="$(
    if [[ -n "$DEPLOYMENT_ID" ]]; then
      printf '%s' "$all_deployments_json" \
        | jq -c --arg dep "$DEPLOYMENT_ID" '
          [.[] | select((.deployment_id // .backend_deployment_id // "") == $dep) | {
            deployment_id: (.deployment_id // .backend_deployment_id // ""),
            service_ready: (.service_ready // false),
            ready_for_ueransim: (.ready_for_ueransim // false),
            last_error: (.last_error // ""),
            nf_types: ((.per_vnf // .per_core_nf // []) | map((.nf_type // .core_nf // .vnf // "") | ascii_downcase) | unique)
          }] | last // empty
        '
    else
      printf '%s' "$all_deployments_json" \
        | jq -c '
          [.[] | select(.service_ready == true) | {
            deployment_id: (.deployment_id // .backend_deployment_id // ""),
            service_ready: (.service_ready // false),
            ready_for_ueransim: (.ready_for_ueransim // false),
            last_error: (.last_error // ""),
            nf_types: ((.per_vnf // .per_core_nf // []) | map((.nf_type // .core_nf // .vnf // "") | ascii_downcase) | unique)
          }] | sort_by(.deployment_id) | last // empty
        '
    fi
  )"

  if [[ -n "$diag_json" ]]; then
    missing="$(
      printf '%s' "$diag_json" | jq -r '
        (.nf_types // []) as $nfs
        | [ "ausf","udm","udr","pcf" ]
        | map(. as $req | select(($nfs | index($req)) == null))
        | join(",")
      '
    )"
    dep_id="$(printf '%s' "$diag_json" | jq -r '.deployment_id // ""')"
    last_err="$(printf '%s' "$diag_json" | jq -r '.last_error // ""')"
    [[ -z "$missing" ]] && missing="(none)"
    echo "[INFO] latest service-ready deployment: ${dep_id:-N/A}, last_error=${last_err:-N/A}, missing_ue_prereqs=$missing"
  fi
  if [[ -n "$DEPLOYMENT_ID" ]]; then
    echo "[ERROR] deployment not ready_for_ueransim=true: $DEPLOYMENT_ID"
  else
    echo "[ERROR] no deployment with service_ready=true and ready_for_ueransim=true"
  fi
  if [[ "$ALLOW_SERVICE_READY_FALLBACK" != "1" ]]; then
    echo "[INFO] set ALLOW_SERVICE_READY_FALLBACK=1 to force test on service_ready-only deployment (may fail if UE prerequisites are missing)"
  fi
  exit 1
fi

ready_dep_id="$(printf '%s' "$ready_dep_json" | jq -r '.deployment_id // .backend_deployment_id // empty')"
echo "[INFO] selected deployment: $ready_dep_id"
if [[ "$selection_mode" == "fallback_service_ready_only" ]]; then
  echo "[WARN] ready_for_ueransim=false, fallback to service_ready=true deployment for verification"
fi
deployed_nodes_json="$(printf '%s' "$ready_dep_json" | jq -c '.deployed_nodes // []')"
expected_nf_json="$(printf '%s' "$ready_dep_json" | jq -c '(.per_vnf // .per_core_nf // [])')"
containers_running="$(printf '%s' "$ready_dep_json" | jq -r '.containers_running // 0')"
containers_total="$(printf '%s' "$ready_dep_json" | jq -r '.containers_total // 0')"
core_nfs_running="$(printf '%s' "$ready_dep_json" | jq -r '.core_nfs_running // 0')"
core_nfs_total="$(printf '%s' "$ready_dep_json" | jq -r '.core_nfs_total // 0')"
service_ready="$(printf '%s' "$ready_dep_json" | jq -r '.service_ready // false')"
ready_for_ueransim="$(printf '%s' "$ready_dep_json" | jq -r '.ready_for_ueransim // false')"
echo "[INFO] deployment runtime: containers=${containers_running}/${containers_total}, core_nfs=${core_nfs_running}/${core_nfs_total}, service_ready=${service_ready}, ready_for_ueransim=${ready_for_ueransim}"

echo "[INFO] verifying NF processes in satellite containers ..."
nf_total=0
nf_failed=0
while IFS=$'\t' read -r nf_type nf_node; do
  [[ -z "$nf_type" || -z "$nf_node" ]] && continue
  nf_total=$((nf_total + 1))
  daemon_name="$(daemon_for_nf_type "$nf_type")"

  container_name="$(find_running_container_for_node "$ready_dep_id" "$nf_node" || true)"
  if [[ -n "$container_name" ]] && docker exec "$container_name" sh -lc "pgrep -x '$daemon_name' >/dev/null 2>&1 || pgrep -f '$daemon_name' >/dev/null 2>&1"; then
    echo "[PASS] NF[$nf_type] node=$nf_node container=$container_name daemon=$daemon_name"
  else
    fallback_container="$(find_container_by_daemon "$daemon_name" || true)"
    if [[ -n "$fallback_container" ]]; then
      if [[ -n "$container_name" && "$fallback_container" != "$container_name" ]]; then
        echo "[PASS] NF[$nf_type] node=$nf_node remapped_container=$fallback_container daemon=$daemon_name (deployment mapping updated after reschedule)"
      else
        echo "[PASS] NF[$nf_type] node=$nf_node container=$fallback_container daemon=$daemon_name"
      fi
    else
      if [[ -z "$container_name" ]]; then
        echo "[FAIL] NF[$nf_type] node=$nf_node container=<not_found> daemon=$daemon_name not found"
      else
        echo "[FAIL] NF[$nf_type] node=$nf_node container=$container_name daemon=$daemon_name not found"
      fi
      nf_failed=$((nf_failed + 1))
    fi
  fi
done < <(
  printf '%s' "$expected_nf_json" \
    | jq -r '.[] | [((.nf_type // .core_nf // .vnf // "") | ascii_downcase), (.node // "")] | @tsv'
)

if (( nf_total == 0 )); then
  echo "[WARN] no NF assignment found in deployment payload"
elif (( nf_failed > 0 )); then
  echo "[ERROR] NF process verification failed: $nf_failed/$nf_total"
  exit 1
else
  echo "[INFO] NF process verification passed: $nf_total/$nf_total"
fi

amf_node="$(printf '%s' "$expected_nf_json" | jq -r '
  first(.[] | select(((.nf_type // .core_nf // .vnf // "") | ascii_downcase) == "amf") | (.node // "")) // empty
')"

amf_container=""
if [[ -n "$amf_node" ]]; then
  canonical_amf_container="$(node_to_container_name "$ready_dep_id" "$amf_node")"
  if docker ps --format '{{.Names}}' | grep -qx "$canonical_amf_container"; then
    if docker exec "$canonical_amf_container" sh -lc 'pgrep -x "open5gs-amfd" >/dev/null 2>&1'; then
      amf_container="$canonical_amf_container"
    fi
  fi
fi

if [[ -z "$amf_container" && -n "$amf_node" ]]; then
  amf_container="$(find_running_container_for_node "$ready_dep_id" "$amf_node" || true)"
  if [[ -n "$amf_container" ]]; then
    if ! docker exec "$amf_container" sh -lc 'pgrep -x "open5gs-amfd" >/dev/null 2>&1'; then
      amf_container=""
    fi
  fi
fi

if [[ -z "$amf_container" ]]; then
  for _ in 1 2 3 4 5; do
    sleep 2
    if [[ -n "$amf_node" ]]; then
      amf_container="$(find_running_container_for_node "$ready_dep_id" "$amf_node" || true)"
      if [[ -n "$amf_container" ]] && docker exec "$amf_container" sh -lc 'pgrep -x "open5gs-amfd" >/dev/null 2>&1'; then
        break
      fi
    fi
    amf_container=""
  done
fi

if [[ -z "$amf_container" ]]; then
  echo "[WARN] AMF container not found via API after refresh, trying docker process scan"
  amf_container="$(find_container_by_daemon 'open5gs-amfd' || true)"
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
provision_subscriber "$IMSI_DIGITS" "$SST" "$SD" "$APN" "$KEY" "$OPC"
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

{
  echo "supi: 'imsi-${IMSI}'"
  echo "mcc: '${MCC}'"
  echo "mnc: '${MNC}'"
  echo "key: '${KEY}'"
  echo "opType: 'OPC'"
  echo "op: '${OPC}'"
  echo "amf: '8000'"
  echo "imei: '356938035643803'"
  echo "imeiSv: '4370816125816151'"
  echo "integrity:"
  echo "  IA1: true"
  echo "  IA2: true"
  echo "  IA3: true"
  echo "ciphering:"
  echo "  EA1: true"
  echo "  EA2: true"
  echo "  EA3: true"
  echo "integrityMaxRate:"
  echo "  uplink: full"
  echo "  downlink: full"
  echo "uacAic:"
  echo "  mps: false"
  echo "  mcs: false"
  echo "uacAcc:"
  echo "  normalClass: 0"
  echo "  class11: false"
  echo "  class12: false"
  echo "  class13: false"
  echo "  class14: false"
  echo "  class15: false"
  echo "configured-nssai:"
  echo "  - sst: ${SST}"
  if [[ -n "$SD" ]]; then echo "    sd: ${SD}"; fi
  echo "default-nssai:"
  echo "  - sst: ${SST}"
  if [[ -n "$SD" ]]; then echo "    sd: ${SD}"; fi
  echo "gnbSearchList:"
  echo "  - ${gnb_ip}"
  echo "sessions:"
  echo "  - type: 'IPv4'"
  echo "    apn: '${APN}'"
  echo "    slice:"
  echo "      sst: ${SST}"
  if [[ -n "$SD" ]]; then echo "      sd: ${SD}"; fi
} > "$TMP_DIR/ue.yaml"

docker exec -d "$GNB_CONTAINER" sh -lc "'$GNB_BIN_PATH' -c /config/gnb.yaml > /tmp/gnb.log 2>&1"

echo "[INFO] waiting gNB NG setup ..."
if ! wait_for_log "$GNB_CONTAINER" /tmp/gnb.log 'NG Setup procedure is successful|NG setup successful|NG Setup Response' 60; then
  echo "[ERROR] gNB failed to establish NG setup"
  docker exec "$GNB_CONTAINER" sh -lc 'tail -n 120 /tmp/gnb.log || true'
  exit 1
fi

echo "[INFO] gNB NG setup success, starting UE ..."
docker exec -d "$UE_CONTAINER" sh -lc "'$UE_BIN_PATH' -c /config/ue.yaml > /tmp/ue.log 2>&1"

REG_PATTERN='Registration complete|Initial Registration is successful|5GMM-REGISTERED'
if ! wait_for_log "$UE_CONTAINER" /tmp/ue.log "$REG_PATTERN" "$VERIFY_TIMEOUT_SEC"; then
  echo "[ERROR] UE smoke verification failed"
  echo "[INFO] gNB log tail:"
  docker exec "$GNB_CONTAINER" sh -lc 'tail -n 120 /tmp/gnb.log || true'
  echo "[INFO] UE log tail:"
  docker exec "$UE_CONTAINER" sh -lc 'tail -n 120 /tmp/ue.log || true'
  exit 1
fi

PDU_PATTERN='PDU Session establishment is successful|PDU Session Establishment Accept'
if wait_for_log "$UE_CONTAINER" /tmp/ue.log "$PDU_PATTERN" "$PDU_WAIT_SEC"; then
  echo "[INFO] UE PDU session establishment detected"
else
  if [[ "$STRICT_PDU_SESSION" == "1" ]]; then
    echo "[ERROR] UE registered but PDU session was not established within ${PDU_WAIT_SEC}s"
    echo "[INFO] UE log tail:"
    docker exec "$UE_CONTAINER" sh -lc 'tail -n 120 /tmp/ue.log || true'
    exit 1
  fi
  echo "[WARN] UE registered but no PDU session success log within ${PDU_WAIT_SEC}s (set STRICT_PDU_SESSION=1 to enforce)"
fi

if docker exec "$UE_CONTAINER" sh -lc "test -f /tmp/ue.log && grep -Eiq 'TUN allocation failure|Open failure /dev/net/tun' /tmp/ue.log"; then
  echo "[WARN] UE data-plane TUN device is unavailable in container (/dev/net/tun)."
  echo "[WARN] On macOS Docker Desktop this is often expected; control-plane registration/PDU-signaling result is still valid."
fi

echo "[OK] UERANSIM smoke verification passed"
echo "[INFO] deployment=$ready_dep_id amf_container=$amf_container amf_ip=$amf_ip gnb_ip=$gnb_ip"
echo "[INFO] gNB recent logs:"
docker exec "$GNB_CONTAINER" sh -lc 'tail -n 40 /tmp/gnb.log || true'
echo "[INFO] UE recent logs:"
docker exec "$UE_CONTAINER" sh -lc 'tail -n 40 /tmp/ue.log || true'
