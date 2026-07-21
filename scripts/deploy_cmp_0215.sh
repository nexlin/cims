#!/usr/bin/env bash
# cmp 0.2.15 업그레이드 원샷 (deployment id=3, package id=58) — 라이브 CMP 재시작 포함.
# 사용법: bash scripts/deploy_cmp_0215.sh
set -euo pipefail

OAM=https://127.0.0.1:4419/api/v1

TOKEN=$(curl -sk -X POST $OAM/auth/login -H 'Content-Type: application/json' \
  -d '{"login_id":"admin","password":"1234"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
echo "[1/3] 로그인 OK"

curl -sk -X PUT $OAM/deployments/3 -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"package_id":58}' | head -c 300
echo; echo "[2/3] deployment 3 → package 58 (cmp 0.2.15)"

curl -sk -X POST $OAM/deployments/3/job -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"job_type":"upgrade"}' | head -c 300
echo; echo "[3/3] upgrade job 발행 — 상태 폴링"

for i in $(seq 1 30); do
    sleep 2
    V=$(curl -sk $OAM/deployments/3 -H "Authorization: Bearer $TOKEN" \
        | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["package_version"],d["status"])')
    echo "  poll $i: $V"
    [[ "$V" == "0.2.15 running" ]] && { echo "== cmp 0.2.15 라이브 확인 =="; exit 0; }
done
echo "!! 30회 폴링 내 미완 — 콘솔 job 로그 확인 필요" >&2
exit 1
