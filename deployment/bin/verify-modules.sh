#!/usr/bin/env bash
#
# ⚠ DEPRECATED (2026-05-15) — 이 스크립트는 새 `verify.py` 가 대체합니다.
# 새 사용:
#   ./bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase smoke
# 이 스크립트는 옛 수동 검증 가이드 — 첫 사용자 학습용으로 보존.
#
# deployment/bin/verify-modules.sh — sim-a 에서 cspsim 으로 PTT/VoLTE 검증
#
# 전제: deploy-modules.sh 완료 + ctrl-a 가 VIP 10.0.1.13 보유
#
# 검증 항목:
#   (1) sim-a → VIP ping
#   (2) PTT REGISTER (1 session)
#   (3) PTT 1대1 호 (2 session, scenario=call)
#   (4) VoLTE 검증 — 옵션 (user JSON seed 필요, sub-issue)

set -euo pipefail

SIM_NS="${SIM_NS:-sim-a}"
SIM_LOCAL_IP="${SIM_LOCAL_IP:-10.0.1.31}"
VIP="${VIP:-10.0.1.13}"
CSPSIM_DIR="/home/nex/work/cims/build/dist/netns-agents/${SIM_NS}/install/modules/cspsim/0.0.1/CSPSIM/cspsim"
RUN_VOLTE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --volte) RUN_VOLTE=1; shift ;;
        --sim-ns) SIM_NS="$2"; shift 2 ;;
        --vip) VIP="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# sudo askpass 가 필요 — 부재 시 동작 안 함 안내
if [[ -z "${SUDO_ASKPASS:-}" ]]; then
    if ! sudo -n true 2>/dev/null; then
        echo "[error] sudo 권한 필요 — passwordless sudo 또는 SUDO_ASKPASS=/path env 필요" >&2
        exit 2
    fi
fi

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

ns_run() { sudo -A ip netns exec "$SIM_NS" sudo -u nex bash -c "$1"; }

# (1) ping
log "(1) sim-a → VIP ($VIP) ping"
if ! sudo -A ip netns exec "$SIM_NS" ping -c 2 -W 1 "$VIP" >/dev/null 2>&1; then
    echo "[error] VIP unreachable. keepalived/cims-health 점검 필요" >&2
    exit 3
fi
echo "  ok"

# (2) PTT REGISTER
log "(2) PTT REGISTER (1 session)"
OUT=$(ns_run "
    cd '$CSPSIM_DIR'
    ./bin/cspsim -server_ip $VIP -local_ip $SIM_LOCAL_IP -count 1 \
        -user +82571900001 -domain ptt.mnc033.mcc450.3gppnetwork.org \
        -password 123456 -mode ptt -scenario register 2>&1
")
echo "$OUT" | grep -E "Registered|fail" | head -3
if ! echo "$OUT" | grep -qE "Registered\s*:\s*1\s*/\s*1"; then
    echo "[error] PTT REGISTER failed" >&2
    echo "$OUT" | tail -20
    exit 4
fi

# (3) PTT 1대1 호
log "(3) PTT 1대1 호 (count=2, scenario=call, duration=5s)"
OUT=$(ns_run "
    cd '$CSPSIM_DIR'
    ./bin/cspsim -server_ip $VIP -local_ip $SIM_LOCAL_IP -count 2 \
        -user +82571900001 -domain ptt.mnc033.mcc450.3gppnetwork.org \
        -password 123456 -mode ptt -scenario call -call_duration 5 2>&1
")
echo "$OUT" | grep -E "STATISTICS|Registered|Call OK|Setup" | head -5
if ! echo "$OUT" | grep -qE "Call OK/End\s*:\s*2"; then
    echo "[error] PTT 1대1 호 failed" >&2
    exit 5
fi

# (4) VoLTE — 옵션
if [[ $RUN_VOLTE -eq 1 ]]; then
    log "(4) VoLTE REGISTER + call (count=2)"
    OUT=$(ns_run "
        cd '$CSPSIM_DIR'
        ./bin/cspsim -server_ip $VIP -local_ip $SIM_LOCAL_IP -count 2 \
            -user 450033100000001 \
            -auth_id 450033100000001@ims.mnc033.mcc450.3gppnetwork.org \
            -domain ims.mnc033.mcc450.3gppnetwork.org \
            -password 123456 -mode volte -scenario call -call_duration 5 2>&1
    ")
    echo "$OUT" | grep -E "STATISTICS|Registered|Call OK|Setup" | head -5
fi

log "✓ 검증 완료"
