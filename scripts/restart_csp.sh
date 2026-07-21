#!/usr/bin/env bash
# CSP(deployment id=2) 라이브 재시작 — g001 세션 상태 초기화용.
# 통화 중 세션이 있으면 끊긴다. 사용법: bash scripts/restart_csp.sh
set -euo pipefail
OAM=https://127.0.0.1:4419/api/v1

TOKEN=$(curl -sk -X POST $OAM/auth/login -H 'Content-Type: application/json' \
  -d '{"login_id":"admin","password":"1234"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
echo "[1/2] 로그인 OK"

curl -sk -X POST $OAM/deployments/2/job -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"job_type":"restart"}' | head -c 300
echo; echo "[2/2] restart job 발행 — 상태 폴링"

for i in $(seq 1 30); do
    sleep 2
    S=$(curl -sk $OAM/deployments/2 -H "Authorization: Bearer $TOKEN" \
        | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["package_version"],d["status"])')
    echo "  poll $i: $S"
    [[ "$S" == "0.2.33 running" ]] && { echo "== CSP 재시작 완료 =="; exit 0; }
done
echo "!! 폴링 내 미완 — 콘솔 job 로그 확인" >&2; exit 1
