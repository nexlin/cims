#!/usr/bin/env bash
#
# ⚠ DEPRECATED (2026-05-15) — 이 스크립트는 새 `apply.py` 가 대체합니다.
# 새 워크플로 (한 명령 end-to-end):
#   ./bin/apply.py --env tb-netns-4-node --scenario volte-ptt --backup --restart auto --verify
# 이 스크립트는 옛 수동 deploy 가이드용 — 초기 enroll/install 단계는 유효.
#
# deployment/bin/deploy-modules.sh — TB-CSC API 기반 모듈 일괄 배포 + start
#
# 전제:
#   1. TB-CSC 가 https://127.0.0.1:4419 에서 동작 중
#   2. ns + agent 들이 이미 enroll/approve 됨 (이 스크립트는 deployment 만 처리)
#      → 초기 셋업은 별도: verify/scripts/ha-netns-up.sh 후
#         verify/scripts/ha-netns-install-agent.sh <ns> <token> <name>
#   3. ha_groups: Control-Server(AS), Media-Server(AA) 정의됨
#
# 토폴로지:
#   Control-Server-01/02 ← csp, isp, psp, csc            (active_standby)
#   Media-Server-01/02   ← cmp, imp, pmp                 (all_active)
#   Simulator-Server-01  ← cspsim (install 만, daemon 아님)  (standalone)
#
# 사용:
#   ./deployment/bin/deploy-modules.sh [--csc-url URL] [--skip-install] [--skip-start]
#
# 멱등성: 이미 생성된 deployment 는 skip. 이미 running 인 dep 은 start 재호출 무영향.

set -euo pipefail

CSC_URL="${CIMS_CSC_URL:-https://127.0.0.1:4419}"
SKIP_INSTALL=0
SKIP_START=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --csc-url) CSC_URL="$2"; shift 2 ;;
        --skip-install) SKIP_INSTALL=1; shift ;;
        --skip-start) SKIP_START=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
api() { curl -sk "$@" "$CSC_URL"; }

# ─── 1. 사전 점검 ──────────────────────────────────────────────────────
log "CSC reachability check (${CSC_URL})"
if ! curl -sk -m 5 "$CSC_URL/health" | grep -q '"ok"'; then
    echo "[error] CSC unreachable at $CSC_URL" >&2
    exit 1
fi

# ─── 2. agents / packages 인벤토리 ─────────────────────────────────────
log "Inventory: agents + packages"
AGENTS=$(curl -sk "$CSC_URL/api/v1/agents" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for a in data.get('items', []):
    print(f\"{a['id']}\t{a['name']}\t{a.get('status','?')}\")
")
PKGS=$(curl -sk "$CSC_URL/api/v1/packages" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for p in data.get('items', []):
    print(f\"{p['id']}\t{p['name']}\")
")
echo "$AGENTS"
echo "---"
echo "$PKGS"

# pkg name → id lookup
pkg_id() { echo "$PKGS" | awk -F'\t' -v n="$1" '$2==n{print $1; exit}'; }
# agent name → id lookup
agent_id() { echo "$AGENTS" | awk -F'\t' -v n="$1" '$2==n{print $1; exit}'; }

# ─── 3. 배포 매핑 정의 ─────────────────────────────────────────────────
# agent_name : space-separated 'pkg:PROCESS_NAME' tuples
declare -A LAYOUT=(
    [Control-Server-01]="csp:CSP isp:ISP psp:PSP csc:CSC"
    [Control-Server-02]="csp:CSP isp:ISP psp:PSP csc:CSC"
    [Media-Server-01]="cmp:CMP imp:IMP pmp:PMP"
    [Media-Server-02]="cmp:CMP imp:IMP pmp:PMP"
    [Simulator-Server-01]="cspsim:CSPSIM"
)

# cspsim 은 daemon 이 아니라 install 만 — start 대상 제외
NON_DAEMON_PKGS="cspsim"

# ─── 4. 기존 deployments 조회 ──────────────────────────────────────────
EXISTING=$(curl -sk "$CSC_URL/api/v1/deployments" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for d in data.get('items', []):
    print(f\"{d['agent_id']}_{d['package_id']}\t{d['id']}\t{d.get('status','?')}\")
")
has_dep() { echo "$EXISTING" | awk -F'\t' -v k="$1" '$1==k{print $2; exit}'; }

# ─── 5. 누락된 deployment 생성 ─────────────────────────────────────────
log "Step 1/3: Create missing deployments"
CREATED_IDS=()
for ag_name in "${!LAYOUT[@]}"; do
    aid=$(agent_id "$ag_name")
    if [[ -z "$aid" ]]; then
        echo "  [warn] agent '$ag_name' not found — skip"
        continue
    fi
    for entry in ${LAYOUT[$ag_name]}; do
        pkg="${entry%%:*}"; proc="${entry##*:}"
        pid=$(pkg_id "$pkg")
        if [[ -z "$pid" ]]; then
            echo "  [warn] package '$pkg' not found — skip"
            continue
        fi
        if [[ -n "$(has_dep "${aid}_${pid}")" ]]; then
            echo "  - $ag_name × $pkg: existing dep=$(has_dep "${aid}_${pid}")"
            continue
        fi
        resp=$(curl -sk -X POST -H "Content-Type: application/json" \
            -d "{\"agent_id\":$aid,\"package_id\":$pid,\"process_name\":\"$proc\"}" \
            "$CSC_URL/api/v1/deployments")
        did=$(echo "$resp" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))" 2>/dev/null || echo "")
        if [[ -n "$did" ]]; then
            echo "  + $ag_name × $pkg ($proc): created dep=$did"
            CREATED_IDS+=("$did")
        else
            echo "  ! $ag_name × $pkg: failed — $resp"
        fi
    done
done

# 5b. 인벤토리 재조회 (status 갱신)
DEPLOYS=$(curl -sk "$CSC_URL/api/v1/deployments" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for d in data.get('items', []):
    print(f\"{d['id']}\t{d.get('agent_name','?')}\t{d['package_name']}\t{d['status']}\")
")

# ─── 6. install (status != stopped/running) ────────────────────────────
if [[ $SKIP_INSTALL -eq 0 ]]; then
    log "Step 2/3: Install where needed (status=pending)"
    while IFS=$'\t' read -r did agent pkg status; do
        [[ -z "$did" ]] && continue
        if [[ "$status" == "pending" ]]; then
            echo -n "  install dep=$did ($agent/$pkg): "
            curl -sk -X POST -H "Content-Type: application/json" \
                -d '{"job_type":"install"}' \
                "$CSC_URL/api/v1/deployments/$did/job" | head -c 80; echo
        fi
    done <<<"$DEPLOYS"
    log "wait 25s for installs"
    sleep 25
fi

# ─── 7. start (daemon 패키지만) ─────────────────────────────────────────
if [[ $SKIP_START -eq 0 ]]; then
    DEPLOYS=$(curl -sk "$CSC_URL/api/v1/deployments" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for d in data.get('items', []):
    print(f\"{d['id']}\t{d.get('agent_name','?')}\t{d['package_name']}\t{d['status']}\")
")
    log "Step 3/3: Start daemon deployments (status=stopped)"
    while IFS=$'\t' read -r did agent pkg status; do
        [[ -z "$did" ]] && continue
        # cspsim 은 daemon 아님 — skip
        if grep -q -w "$pkg" <<<"$NON_DAEMON_PKGS"; then
            echo "  - dep=$did ($agent/$pkg): non-daemon, skip start"
            continue
        fi
        if [[ "$status" == "stopped" ]]; then
            echo -n "  start dep=$did ($agent/$pkg): "
            curl -sk -X POST -H "Content-Type: application/json" \
                -d '{"job_type":"start"}' \
                "$CSC_URL/api/v1/deployments/$did/job" | head -c 80; echo
        fi
    done <<<"$DEPLOYS"
    log "wait 15s for starts"
    sleep 15
fi

# ─── 8. 최종 상태 ──────────────────────────────────────────────────────
log "Final status:"
curl -sk "$CSC_URL/api/v1/deployments" | python3 -c "
import sys, json
data = json.load(sys.stdin)
ok = 0; ng = 0
for d in sorted(data.get('items', []), key=lambda x: (x.get('agent_name',''), x['package_name'])):
    s = d.get('status','?')
    mark = '✓' if s == 'running' else ('·' if s == 'stopped' else 'x')
    print(f\"  {mark} dep={d['id']:>3} {d.get('agent_name','?'):<22} {d['package_name']:<8} {s}\")
    if s == 'running': ok += 1
    else: ng += 1
print(f'\\nrunning={ok}  others={ng}')
"
