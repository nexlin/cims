#!/usr/bin/env bash
# deployment/bin/health.sh — 빠른 LIVE 진단 (sudo 불필요).
#
# 점검:
#   1. CSC API 응답 (/api/v1/agents)
#   2. csp/cmp 프로세스 살아있음 (host pgrep)
#   3. cmp.log 의 최근 LISTEN 라인
#   4. csp.log 의 endpoint registered 라인
#
# 사용:
#   ./health.sh                       # 기본 (build/dist 의 csp/cmp)
#   ./health.sh --csc <url>           # CSC API URL override
#   ./health.sh --base <dist-dir>     # csp/cmp install base override

set -eu

CSC_URL="https://127.0.0.1:4419"
DIST_BASE="/home/nex/work/cims/build/dist"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --csc) CSC_URL="$2"; shift 2 ;;
        --base) DIST_BASE="$2"; shift 2 ;;
        -h|--help)
            sed -n '1,14p' "$0" | tail -13; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

ok()   { printf ' \033[32m✓\033[0m %s\n' "$*"; }
fail() { printf ' \033[31m✗\033[0m %s\n' "$*"; }
warn() { printf ' \033[33m!\033[0m %s\n' "$*"; }
hdr()  { printf '\n\033[1m── %s ──\033[0m\n' "$*"; }

ANY_FAIL=0

# 1) CSC API
hdr "CSC API"
if curl -sk --max-time 5 "$CSC_URL/api/v1/agents" >/dev/null 2>&1; then
    SUMMARY=$(curl -sk --max-time 5 "$CSC_URL/api/v1/agents" 2>/dev/null | python3 -c "
import json,sys
data=json.load(sys.stdin)
items=data.get('items',[])
on=[i['name'] for i in items if i.get('status')=='online']
off=[i['name'] for i in items if i.get('status')!='online']
print(f\"online={len(on)} ({','.join(on)}) offline={len(off)}\" + (f\" ({','.join(off)})\" if off else ''))
" 2>&1)
    ok "$CSC_URL : $SUMMARY"
else
    fail "$CSC_URL : 응답 없음"; ANY_FAIL=1
fi

# 2) Deployment status
hdr "Deployment status (csp/cmp)"
curl -sk --max-time 5 "$CSC_URL/api/v1/deployments" 2>/dev/null | python3 -c "
import json,sys
data=json.load(sys.stdin)
for d in data.get('items',[]):
    if d.get('package_name') in ('csp','cmp'):
        st=d.get('status','?')
        mk='\033[32m✓\033[0m' if st=='running' else '\033[31m✗\033[0m'
        print(f'  {mk} id={d[\"id\"]:>3} {d.get(\"agent_name\",\"?\"):20s} {d.get(\"package_name\",\"?\"):5s} status={st}')
" 2>/dev/null

# 3) Host process — argv 가 'bin/<pkg> config/<pkg>.json' 형식
hdr "Host process"
for pkg in csp cmp; do
    PIDS=$(pgrep -f "bin/$pkg config" | tr '\n' ',' | sed 's/,$//')
    if [[ -n "$PIDS" ]]; then
        ok "$pkg PIDs: $PIDS"
    else
        fail "$pkg 프로세스 없음"; ANY_FAIL=1
    fi
done

# 4) cmp.log 의 LISTEN
hdr "cmp LISTEN"
LOG="$DIST_BASE/cmp/log/cmp.log"
if [[ -f "$LOG" ]]; then
    LINE=$(grep -E "Server listening" "$LOG" | tail -1)
    [[ -n "$LINE" ]] && ok "cmp : ${LINE##*]}" || warn "cmp : Server listening 라인 없음"
else
    warn "cmp : log 없음 ($LOG)"
fi

# 5) csp.log 의 AddEndpoint
hdr "csp CmpClient endpoints"
LOGDIR="$DIST_BASE/csp/log"
LATEST=$(ls -t "$LOGDIR"/csp_*.log 2>/dev/null | head -1)
if [[ -n "$LATEST" ]]; then
    LINE=$(grep -E "registered.*additional endpoints" "$LATEST" | tail -1)
    [[ -n "$LINE" ]] && ok "csp : ${LINE##*]}" || warn "csp : AddEndpoint 라인 없음 — 옛 binary?"
else
    warn "csp : log 없음 ($LOGDIR)"
fi

echo ""
if [[ $ANY_FAIL -eq 0 ]]; then
    printf '\033[1;32mOVERALL: HEALTHY\033[0m\n'
    exit 0
else
    printf '\033[1;31mOVERALL: ISSUES FOUND\033[0m\n'
    exit 1
fi
