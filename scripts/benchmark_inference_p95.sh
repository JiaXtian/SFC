#!/usr/bin/env bash
set -euo pipefail

API_BASE="${API_BASE:-http://127.0.0.1:18080/api/v1}"
USERNAME="${USERNAME:-admin}"
PASSWORD="${PASSWORD:-123456}"
ROUNDS="${ROUNDS:-30}"
TOPK="${TOPK:-6}"

if ! command -v jq >/dev/null 2>&1; then
  echo "[ERROR] jq is required"
  exit 1
fi

login_payload=$(jq -nc --arg u "$USERNAME" --arg p "$PASSWORD" '{username:$u,password:$p}')
TOKEN=$(curl -fsS -X POST "$API_BASE/auth/login" -H 'Content-Type: application/json' -d "$login_payload" | jq -r '.token // empty')
if [[ -z "$TOKEN" ]]; then
  echo "[ERROR] login failed"
  exit 1
fi

TOPO=$(curl -fsS "$API_BASE/topology")
NODE_COUNT=$(echo "$TOPO" | jq -r '.topology.nodes | length')
if [[ "$NODE_COUNT" -lt 3 ]]; then
  echo "[ERROR] topology has too few nodes: $NODE_COUNT"
  exit 1
fi

SRC=$(echo "$TOPO" | jq -r '.topology.nodes[0].id')
DST=$(echo "$TOPO" | jq -r '.topology.nodes[(.topology.nodes|length)/2|floor].id')
if [[ -z "$SRC" || -z "$DST" || "$SRC" == "$DST" ]]; then
  DST=$(echo "$TOPO" | jq -r '.topology.nodes[-1].id')
fi
if [[ -z "$DST" || "$SRC" == "$DST" ]]; then
  echo "[ERROR] failed to select distinct source/destination"
  exit 1
fi

tmp_lat=$(mktemp)
tmp_feasible=$(mktemp)
trap 'rm -f "$tmp_lat" "$tmp_feasible"' EXIT

ok_count=0
for ((i=1; i<=ROUNDS; i++)); do
  request_id="bench_$(date +%s)_$i"
  payload=$(jq -nc \
    --arg req "$request_id" \
    --arg src "$SRC" \
    --arg dst "$DST" \
    --argjson topk "$TOPK" \
    '{
      request_id:$req,
      source_node:$src,
      destination_node:$dst,
      topk:$topk,
      constraints:{max_latency_ms:260,min_bandwidth_gbps:0.4,min_reliability:0.62},
      core_nfs:[
        {name:"nrf",nf_type:"nrf"},
        {name:"ausf",nf_type:"ausf"},
        {name:"udm",nf_type:"udm"},
        {name:"udr",nf_type:"udr"},
        {name:"amf",nf_type:"amf"},
        {name:"smf",nf_type:"smf"},
        {name:"upf",nf_type:"upf"}
      ]
    }')

  resp=$(curl -sS -X POST "$API_BASE/sfc/plan" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "$payload" || true)
  inference_ms=$(echo "$resp" | jq -r '.inference_time_ms // empty')
  deployable=$(echo "$resp" | jq -r '.deployable_count // empty')
  if [[ -n "$inference_ms" ]]; then
    echo "$inference_ms" >> "$tmp_lat"
    [[ -n "$deployable" ]] && echo "$deployable" >> "$tmp_feasible"
    ok_count=$((ok_count+1))
  fi
done

if [[ "$ok_count" -eq 0 ]]; then
  echo "[ERROR] no successful planning samples"
  exit 1
fi

sort -n "$tmp_lat" -o "$tmp_lat"
count=$(wc -l < "$tmp_lat" | tr -d ' ')
p50_idx=$(( (count + 1) / 2 ))
p95_idx=$(( (count * 95 + 99) / 100 ))
p99_idx=$(( (count * 99 + 99) / 100 ))
p50=$(awk -v idx="$p50_idx" 'NR==idx{print; exit}' "$tmp_lat")
p95=$(awk -v idx="$p95_idx" 'NR==idx{print; exit}' "$tmp_lat")
p99=$(awk -v idx="$p99_idx" 'NR==idx{print; exit}' "$tmp_lat")
avg=$(awk '{s+=$1} END{if (NR>0) printf "%.3f", s/NR}' "$tmp_lat")
minv=$(head -n1 "$tmp_lat")
maxv=$(tail -n1 "$tmp_lat")

feasible_avg="n/a"
if [[ -s "$tmp_feasible" ]]; then
  feasible_avg=$(awk '{s+=$1} END{if (NR>0) printf "%.2f", s/NR}' "$tmp_feasible")
fi

echo "[INFO] API=$API_BASE src=$SRC dst=$DST rounds=$ROUNDS success=$ok_count"
echo "[INFO] latency_ms min=$minv p50=$p50 p95=$p95 p99=$p99 max=$maxv avg=$avg"
echo "[INFO] avg_deployable_count=$feasible_avg"
