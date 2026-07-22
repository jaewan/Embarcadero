#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 CAMPAIGN_DIR" >&2
  exit 2
fi

campaign_dir=$1
if [[ ! -d "$campaign_dir" ]]; then
  echo "campaign directory not found: $campaign_dir" >&2
  exit 2
fi

field() {
  local record=$1 key=$2
  sed -nE "s/.*(^|[[:space:]])${key}=([^[:space:]]+).*/\\2/p" <<<"$record"
}

printf '%s\n' 'run_id,target_mbps,control_transport,token_gate_policy,token_delay_us,replication_factor,ack_level,requests,grants,failures,gate_aborts,gate_denied,payload_sends,payload_before_grant,payload_admission_denied,aborted,mean_token_latency_us,ticket_wait_ms_sum'

mapfile -t logs < <(find "$campaign_dir/latency" -type f \
  -path '*/raw/steady/*/CORFU*/run.log' -print | LC_ALL=C sort)
if [[ ${#logs[@]} -eq 0 ]]; then
  echo "no canonical Corfu raw logs found under $campaign_dir/latency" >&2
  exit 1
fi

for log in "${logs[@]}"; do
  record=$(grep -m1 '\[CORFU_TOKEN_PHASE\]' "$log" || true)
  if [[ -z "$record" ]]; then
    echo "missing CORFU_TOKEN_PHASE record: $log" >&2
    exit 1
  fi
  run_id=$(sed -nE 's#^.*/(run_l[^/]+)/points/.*#\1#p' <<<"$log")
  target=$(sed -nE 's#^.*/target_([0-9]+)mbps/.*#\1#p' <<<"$log")
  printf '%s,%s,grpc,cv_fail_closed_v1,%s,2,2,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$run_id" "$target" "$(field "$record" token_delay_us)" \
    "$(field "$record" requests)" "$(field "$record" grants)" \
    "$(field "$record" failures)" "$(field "$record" gate_aborts)" \
    "$(field "$record" gate_denied)" "$(field "$record" payload_sends)" \
    "$(field "$record" payload_before_grant)" \
    "$(field "$record" payload_admission_denied)" "$(field "$record" aborted)" \
    "$(field "$record" mean_token_latency_us)" \
    "$(field "$record" ticket_wait_ms_sum)"
done
