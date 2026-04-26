#!/usr/bin/env bash
set -euo pipefail

API_BASE="${API_BASE:-}"
API_TOKEN="${API_TOKEN:-}"
API_USERNAME="${API_USERNAME:-admin}"
API_PASSWORD="${API_PASSWORD:-123456}"
DEPLOYMENT_ID="${DEPLOYMENT_ID:-}"

RECOVERY_TIMEOUT_SEC="${RECOVERY_TIMEOUT_SEC:-240}"
RECOVERY_POLL_SEC="${RECOVERY_POLL_SEC:-2}"
WAIT_FAULT_TIMEOUT_SEC="${WAIT_FAULT_TIMEOUT_SEC:-600}"

VERIFY_TIMEOUT_SEC="${VERIFY_TIMEOUT_SEC:-120}"
PDU_WAIT_SEC="${PDU_WAIT_SEC:-30}"
STRICT_PDU_SESSION="${STRICT_PDU_SESSION:-1}"
STRICT_TUN_DEVICE="${STRICT_TUN_DEVICE:-1}"
STRICT_UE_IP_ALLOC="${STRICT_UE_IP_ALLOC:-1}"
UE_IP_WAIT_SEC="${UE_IP_WAIT_SEC:-30}"
UE_TUN_IFACE="${UE_TUN_IFACE:-uesimtun0}"
SMOKE_SCRIPT="${SMOKE_SCRIPT:-}"

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "[ERROR] missing command: $cmd"
    exit 1
  fi
}

require_cmd curl
require_cmd jq

if [[ -z "$API_BASE" ]]; then
  for root in "http://127.0.0.1:18080" "http://127.0.0.1:8080"; do
    if curl -fsS "$root/api/v1/health" >/dev/null 2>&1 || curl -fsS "$root/health" >/dev/null 2>&1; then
      API_BASE="$root/api/v1"
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
    exit 1
  fi
fi

CURL_AUTH_ARGS=("-H" "Authorization: Bearer $API_TOKEN")
api_get() {
  local url="$1"
  curl -fsS "${CURL_AUTH_ARGS[@]}" "$url"
}

now_ms() {
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import time; print(int(time.time() * 1000))'
    return 0
  fi
  if command -v perl >/dev/null 2>&1; then
    perl -MTime::HiRes=time -e 'printf("%d\n", int(time()*1000));'
    return 0
  fi
  printf '%s000\n' "$(date +%s)"
}

if [[ -z "$SMOKE_SCRIPT" ]]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  SMOKE_SCRIPT="$SCRIPT_DIR/verify_ueransim_smoke.sh"
fi

if [[ ! -x "$SMOKE_SCRIPT" ]]; then
  echo "[ERROR] smoke script is not executable: $SMOKE_SCRIPT"
  exit 1
fi

echo "[INFO] selecting target deployment from $API_BASE/deployments"
all_deployments_json="$(api_get "$API_BASE/deployments")"
if [[ -n "$DEPLOYMENT_ID" ]]; then
  deployment_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c --arg dep "$DEPLOYMENT_ID" \
        '[.[] | select((.deployment_id // .backend_deployment_id // "") == $dep)] | last // empty'
  )"
else
  deployment_json="$(
    printf '%s' "$all_deployments_json" \
      | jq -c '[.[] | select(.service_ready == true and .ready_for_ueransim == true)] | sort_by(.last_update_at // .deployed_at // "") | last // empty'
  )"
fi

if [[ -z "$deployment_json" ]]; then
  echo "[ERROR] target deployment not found"
  exit 1
fi

DEPLOYMENT_ID="$(printf '%s' "$deployment_json" | jq -r '.deployment_id // .backend_deployment_id // empty')"
SFC_ID="$(printf '%s' "$deployment_json" | jq -r '.sfc_id // .sfc_name // .name // .display_id // empty')"
if [[ -z "$SFC_ID" ]]; then
  SFC_ID="$DEPLOYMENT_ID"
fi

echo "[INFO] target deployment=$DEPLOYMENT_ID sfc=$SFC_ID"
echo "[INFO] baseline verification before fault injection ..."
API_BASE="$API_BASE" API_TOKEN="$API_TOKEN" DEPLOYMENT_ID="$DEPLOYMENT_ID" \
VERIFY_TIMEOUT_SEC="$VERIFY_TIMEOUT_SEC" PDU_WAIT_SEC="$PDU_WAIT_SEC" STRICT_PDU_SESSION="$STRICT_PDU_SESSION" \
STRICT_TUN_DEVICE="$STRICT_TUN_DEVICE" STRICT_UE_IP_ALLOC="$STRICT_UE_IP_ALLOC" \
UE_IP_WAIT_SEC="$UE_IP_WAIT_SEC" UE_TUN_IFACE="$UE_TUN_IFACE" \
KEEP_UERANSIM=0 "$SMOKE_SCRIPT"

baseline_signature="$(printf '%s' "$deployment_json" | jq -r '(.per_vnf // .per_core_nf // []) | map(((.nf_type // .core_nf // .vnf // "") + "@" + (.node // ""))) | sort | join(",")')"
baseline_nodes="$(printf '%s' "$deployment_json" | jq -r '(.deployed_nodes // []) | sort | join(",")')"
echo "[INFO] baseline passed, now waiting for your manual fault injection in control center ..."
echo "[INFO] wait window=${WAIT_FAULT_TIMEOUT_SEC}s, poll=${RECOVERY_POLL_SEC}s"

wait_deadline=$((SECONDS + WAIT_FAULT_TIMEOUT_SEC))
reschedule_started=0
recovered=0
reschedule_start_ms=0
recovery_deadline=-1
latest_phase=""
latest_runtime=""
timeout_reason=""

while (( SECONDS < wait_deadline )); do
  dep_now="$(
    api_get "$API_BASE/deployments" \
      | jq -c --arg dep "$DEPLOYMENT_ID" \
        '[.[] | select((.deployment_id // .backend_deployment_id // "") == $dep)] | last // empty'
  )"
  if [[ -z "$dep_now" ]]; then
    sleep "$RECOVERY_POLL_SEC"
    continue
  fi

  current_phase="$(printf '%s' "$dep_now" | jq -r '.orchestration_phase // ""')"
  current_signature="$(printf '%s' "$dep_now" | jq -r '(.per_vnf // .per_core_nf // []) | map(((.nf_type // .core_nf // .vnf // "") + "@" + (.node // ""))) | sort | join(",")')"
  current_nodes="$(printf '%s' "$dep_now" | jq -r '(.deployed_nodes // []) | sort | join(",")')"
  service_ready="$(printf '%s' "$dep_now" | jq -r '.service_ready // false')"
  ready_for_ueransim="$(printf '%s' "$dep_now" | jq -r '.ready_for_ueransim // false')"
  containers_running="$(printf '%s' "$dep_now" | jq -r '.containers_running // 0')"
  containers_total="$(printf '%s' "$dep_now" | jq -r '.containers_total // 0')"
  core_nfs_running="$(printf '%s' "$dep_now" | jq -r '.core_nfs_running // 0')"
  core_nfs_total="$(printf '%s' "$dep_now" | jq -r '.core_nfs_total // 0')"

  latest_phase="$current_phase"
  latest_runtime="containers=${containers_running}/${containers_total}, core_nfs=${core_nfs_running}/${core_nfs_total}, service_ready=${service_ready}, ready_for_ueransim=${ready_for_ueransim}"

  phase_hint=0
  if [[ "$current_phase" == "stopping_old" || "$current_phase" == "starting_containers" || "$current_phase" == "starting_nfs" || "$current_phase" == "probing" || "$current_phase" == "degraded" ]]; then
    phase_hint=1
  fi

  changed_hint=0
  if [[ "$current_signature" != "$baseline_signature" || "$current_nodes" != "$baseline_nodes" ]]; then
    changed_hint=1
  fi

  if [[ "$reschedule_started" != "1" && ( "$phase_hint" == "1" || "$changed_hint" == "1" || "$service_ready" != "true" ) ]]; then
    reschedule_started=1
    reschedule_start_ms="$(now_ms)"
    if [[ ! "$reschedule_start_ms" =~ ^[0-9]+$ ]]; then
      reschedule_start_ms="$((SECONDS * 1000))"
    fi
    recovery_deadline=$((SECONDS + RECOVERY_TIMEOUT_SEC))
    echo "[INFO] reschedule detected: phase=$current_phase runtime=$latest_runtime"
    echo "[INFO] waiting for post-reschedule service recovery (timeout=${RECOVERY_TIMEOUT_SEC}s) ..."
  fi

  if [[ "$reschedule_started" == "1" \
      && "$service_ready" == "true" && "$ready_for_ueransim" == "true" \
      && "$containers_running" == "$containers_total" \
      && "$core_nfs_running" == "$core_nfs_total" ]]; then
    echo "[INFO] recovered runtime observed, running UE re-attach validation ..."
    if API_BASE="$API_BASE" API_TOKEN="$API_TOKEN" DEPLOYMENT_ID="$DEPLOYMENT_ID" \
      VERIFY_TIMEOUT_SEC="$VERIFY_TIMEOUT_SEC" PDU_WAIT_SEC="$PDU_WAIT_SEC" STRICT_PDU_SESSION="$STRICT_PDU_SESSION" \
      STRICT_TUN_DEVICE="$STRICT_TUN_DEVICE" STRICT_UE_IP_ALLOC="$STRICT_UE_IP_ALLOC" \
      UE_IP_WAIT_SEC="$UE_IP_WAIT_SEC" UE_TUN_IFACE="$UE_TUN_IFACE" \
      KEEP_UERANSIM=0 "$SMOKE_SCRIPT"; then
      recovered=1
      break
    fi
  fi

  if [[ "$reschedule_started" == "1" && "$recovery_deadline" -gt 0 && "$SECONDS" -ge "$recovery_deadline" ]]; then
    timeout_reason="recover_timeout"
    break
  fi

  sleep "$RECOVERY_POLL_SEC"
done

if [[ "$reschedule_started" != "1" ]]; then
  echo "[ERROR] no reschedule detected within wait window (${WAIT_FAULT_TIMEOUT_SEC}s)"
  echo "[INFO] last_phase=${latest_phase:-unknown} runtime=${latest_runtime:-N/A}"
  exit 1
fi

if [[ "$recovered" != "1" ]]; then
  if [[ "$timeout_reason" == "recover_timeout" ]]; then
    echo "[ERROR] service did not recover within timeout (${RECOVERY_TIMEOUT_SEC}s) after reschedule start"
  else
    echo "[ERROR] service did not recover within timeout window"
  fi
  echo "[INFO] last_phase=${latest_phase:-unknown} runtime=${latest_runtime:-N/A}"
  exit 1
fi

recovery_done_ms="$(now_ms)"
if [[ ! "$recovery_done_ms" =~ ^[0-9]+$ ]]; then
  recovery_done_ms="$((SECONDS * 1000))"
fi
recovery_cost_ms=$((recovery_done_ms - reschedule_start_ms))

echo "[OK] reschedule recovery verification passed"
echo "[INFO] deployment=$DEPLOYMENT_ID sfc=$SFC_ID recovery_time_ms=$recovery_cost_ms"
