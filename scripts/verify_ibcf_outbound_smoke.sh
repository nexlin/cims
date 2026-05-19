#!/usr/bin/env bash
# L6 — IBCF 멀티 피어 routes LIVE smoke
#
# 목적:
#   b727167 (route 결정 → outbound leg Via/Contact 자기 주소 동적화) 의 LIVE 효과
#   를 검증. routes 컬렉션에 외부 peer routing rule 1행 셋업 → cspsim 으로
#   외부 user 호 시도 → CSP 가 outbound INVITE 송신 시도 → sip.jsonl tx 메시지
#   에서 Via/Contact 가 route.local_node_ref (csp-main-tcp 25061) 의 bind 값
#   으로 박혔는지 확인.
#
# 사용:
#   ./scripts/verify_ibcf_outbound_smoke.sh [setup|smoke|verify|teardown|all]
#
# 단계 (all):
#   1. setup    — remote_nodes/rules/rule_sets/routes/route_sets/routing_policies PUT
#   2. smoke    — cspsim 으로 외부 user 1콜 시도 (외부 peer 없음 → INVITE tx 만 발생)
#   3. verify   — sip.jsonl 의 outbound INVITE Via/Contact host:port 확인
#   4. teardown — 위 6 컬렉션 모두 빈 records 로 PUT (baseline 복원)
#
# 환경 전제:
#   - csc 4419 + csp ctrl-a/b 모두 LIVE
#   - HA fan-out (T1) 동작 — 1번 PUT 으로 양 멤버 동기
#   - 외부 peer 는 실재 안 함 (10.99.99.99:5060) → INVITE tx 후 timeout 자연 발생

set -euo pipefail
CSC=${CSC:-https://127.0.0.1:4419}
DID=${DID:-36}
CSP_HOST=${CSP_HOST:-10.0.1.13}
CSP_PORT=${CSP_PORT:-5060}
PEER_IP=${PEER_IP:-10.99.99.99}
PEER_PORT=${PEER_PORT:-5060}
SIP_LOG_DIR=${SIP_LOG_DIR:-/home/nex/work/cims/build/dist/ext_mnt/service_log}
CSPSIM=${CSPSIM:-/home/nex/work/cims/build/bin/cspsim}

put() {
  local coll=$1; shift
  local records=$1; shift
  curl -sk -X PUT "$CSC/api/v1/deployments/$DID/collection/$coll" \
    -H 'Content-Type: application/json' \
    -d "{\"records\": $records}" \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"  {sys.argv[1]} → ok={d.get('ok')} propagated={d.get('propagated')} signaled={d.get('signaled',[])}\")" "$coll"
}

cmd_setup() {
  echo "[setup] routes/rules/routing_policies 셋업 — L6 smoke"
  # remote_nodes: 외부 peer 1개 + 기존 cmp-media-a/b 유지
  put remote_nodes '[
    {"id":"cmp-media-a","name":"cmp-media-a","ip":"10.0.1.21","port":9000,"protocol":"UDP","remote_domain":"","srv_lookup":false,"dns_fallback":true,"tls_verify":false,"enabled":true,"tags":["cmp"],"note":"auto cmp member media-a"},
    {"id":"cmp-media-b","name":"cmp-media-b","ip":"10.0.1.22","port":9000,"protocol":"UDP","remote_domain":"","srv_lookup":false,"dns_fallback":true,"tls_verify":false,"enabled":true,"tags":["cmp"],"note":"auto cmp member media-b"},
    {"id":"ext-peer-1","name":"ext-peer-1","ip":"'"$PEER_IP"'","port":'"$PEER_PORT"',"protocol":"UDP","remote_domain":"external.example","srv_lookup":false,"dns_fallback":false,"tls_verify":false,"enabled":true,"tags":["ibcf"],"note":"L6 smoke peer — no real listener"}
  ]'
  put rules '[
    {"id":"rule-to-ext","name":"rule-to-ext","enabled":true,"field":"to_uri_user","op":"prefix","value":"99","tags":[],"note":"L6: To user 가 99 로 시작하면 외부 peer 라우팅"}
  ]'
  put rule_sets '[
    {"id":"rs-to-ext","name":"rs-to-ext","enabled":true,"combinator":"AND","members":["rule-to-ext"],"tags":[],"note":""}
  ]'
  # routes: pair (local_node_ref + remote_node_ref) — b727167 의 핵심 검증 포인트
  put routes '[
    {"id":"route-tcp-to-ext","name":"route-tcp-to-ext","enabled":true,"local_node_ref":"csp-main-tcp","remote_node_ref":"ext-peer-1","outbound_proxy_ip":"","outbound_proxy_port":0,"register_to_remote":false,"register_expires":3600,"auth_user":"","auth_password":"","auth_realm":"","max_concurrent_calls":0,"cps_limit":0,"tags":["ibcf"],"note":"L6 smoke — outbound TCP 25061"}
  ]'
  put route_sets '[
    {"id":"rset-ext","name":"rset-ext","enabled":true,"distribution_policy":"failover","members":["route-tcp-to-ext"],"health_check_mode":"none","health_check_interval_sec":0,"health_check_dead_threshold":0}
  ]'
  put routing_policies '[
    {"id":"pol-ext","name":"pol-ext","enabled":true,"priority":100,"match_rule_set_ref":"rs-to-ext","target_type":"route_set","target_ref":"rset-ext","transform_rule_set_refs":[]}
  ]'
  echo "[setup] 완료 — csp 가 다음 reload 또는 SIGUSR1 시 적용"
  # csp 의 sync_config 가 자동 reload 함 (T1 fan-out 의 일부)
  sleep 2
}

cmd_smoke() {
  echo "[smoke] cspsim — 1콜 외부 user 호출 시도 (외부 peer 응답 안 옴 → tx 만 발생)"
  # 등록된 user 가 외부 user 호 → routing_policies 매칭 → outbound INVITE
  timeout 12 "$CSPSIM" -server_ip "$CSP_HOST" -server_port "$CSP_PORT" \
    -count 1 -user 1001 -domain ims.mnc033.mcc450.3gppnetwork.org -password 1234 \
    -mode volte -scenario call -call_duration 3 \
    -callee_override sip:99001@ims.mnc033.mcc450.3gppnetwork.org \
    -interval 100 < /dev/null 2>&1 | tail -10 || echo "[smoke] cspsim exit (expected — peer never answers)"
}

cmd_verify() {
  echo "[verify] sip.jsonl 의 최근 outbound INVITE 검색"
  local LATEST=$(find "$SIP_LOG_DIR" -name "csp_*_sip.msg.jsonl" -mmin -2 2>/dev/null | sort | tail -1)
  if [ -z "$LATEST" ]; then
    echo "[verify] sip.jsonl 없음"; return 1
  fi
  echo "[verify] file: $LATEST"
  echo "[verify] 최근 5개 TX INVITE 의 Via/Contact:"
  python3 - <<PYEOF
import json, sys
path = "$LATEST"
peer_ip = "$PEER_IP"
hits = []
with open(path) as f:
    for line in f:
        try: d = json.loads(line)
        except: continue
        if d.get('direction') == 'tx' and d.get('method') == 'INVITE':
            msg = d.get('message','')
            # peer_ip 으로 라우팅된 메시지만 (b727167 효과 검증)
            if peer_ip in msg:
                hits.append((d.get('ts'), msg))
print(f"  outbound INVITE → {peer_ip} 매치 수: {len(hits)}")
for ts, msg in hits[-3:]:
    lines = msg.split('\n')
    via = next((l for l in lines if l.startswith('Via:')), '')
    contact = next((l for l in lines if l.startswith('Contact:')), '')
    print(f"  ts={ts}")
    print(f"    {via.strip()}")
    print(f"    {contact.strip()}")
    print()
PYEOF
}

cmd_teardown() {
  echo "[teardown] 모든 추가 컬렉션 비우기 (baseline 복원)"
  put routing_policies '[]'
  put route_sets '[]'
  put routes '[]'
  put rule_sets '[]'
  put rules '[]'
  put remote_nodes '[
    {"id":"cmp-media-a","name":"cmp-media-a","ip":"10.0.1.21","port":9000,"protocol":"UDP","remote_domain":"","srv_lookup":false,"dns_fallback":true,"tls_verify":false,"enabled":true,"tags":["cmp"],"note":"auto cmp member media-a"},
    {"id":"cmp-media-b","name":"cmp-media-b","ip":"10.0.1.22","port":9000,"protocol":"UDP","remote_domain":"","srv_lookup":false,"dns_fallback":true,"tls_verify":false,"enabled":true,"tags":["cmp"],"note":"auto cmp member media-b"}
  ]'
  echo "[teardown] 완료"
}

case "${1:-all}" in
  setup)    cmd_setup ;;
  smoke)    cmd_smoke ;;
  verify)   cmd_verify ;;
  teardown) cmd_teardown ;;
  all)
    cmd_setup
    cmd_smoke
    sleep 2
    cmd_verify
    cmd_teardown
    ;;
  *) echo "사용: $0 [setup|smoke|verify|teardown|all]"; exit 1 ;;
esac
