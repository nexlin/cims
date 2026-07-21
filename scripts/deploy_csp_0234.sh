#!/usr/bin/env bash
# CSP 0.2.34 배포 — OnCallStarted JoinGroup NOT_FOUND self-heal(AddGroup 재수립+재시도).
# 사용법: bash scripts/deploy_csp_0234.sh
set -euo pipefail
OAM=https://127.0.0.1:4419/api/v1
PKG=/home/cims/work/cims/build/dist/packages/csp-0.2.34.tar.gz
DEP=2   # csp deployment id

TOKEN=$(curl -sk -X POST $OAM/auth/login -H 'Content-Type: application/json' \
  -d '{"login_id":"admin","password":"1234"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
echo "[1/4] login OK"

PID=$(curl -sk -X POST $OAM/packages -H "Authorization: Bearer $TOKEN" \
  -F "file=@$PKG" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d.get("id") or d.get("package_id") or d)')
echo "[2/4] uploaded package id=$PID"

curl -sk -X PUT $OAM/deployments/$DEP -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d "{\"package_id\":$PID}" | head -c 300; echo
echo "[3/4] deployment target set → package_id=$PID"

curl -sk -X POST $OAM/deployments/$DEP/job -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"job_type":"upgrade"}' | head -c 300; echo
echo "[4/4] upgrade job issued — polling"
for i in $(seq 1 40); do
  sleep 2
  S=$(curl -sk $OAM/deployments/$DEP -H "Authorization: Bearer $TOKEN" \
      | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d.get("package_version"),d.get("status"))')
  echo "  poll $i: $S"
  echo "$S" | grep -q "0.2.34 running" && { echo "DONE"; break; }
done
