#!/bin/bash
# notify_psp.sh — keepalived state transition hook for PSP (Phase 1.F: VIP-only cold-spare).
# Invoked by keepalived with args: $1=TYPE, $2=NAME, $3=STATE, $4=PRIORITY.
# 1.D-1: MASTER 승격 시 Redis register replay 는 cims.sh start psp 가 처리 (기동 시점).

TYPE="$1"
NAME="$2"
STATE="$3"
PRIO="$4"

LOG="${HA_LOG_DIR:-/var/log/cims-ha}/notify_psp.log"
UNIT="${HA_UNIT_PSP:-cims-psp}"
mkdir -p "$(dirname "$LOG")" 2>/dev/null

log() { echo "$(date -Iseconds) ${TYPE} ${NAME} -> ${STATE} (prio=${PRIO}) :: $*" >> "$LOG"; }

case "$STATE" in
    MASTER)
        log "MASTER 승격 → systemctl start ${UNIT}"
        systemctl start "${UNIT}" 2>>"$LOG" || log "FAIL: systemctl start ${UNIT}"
        ;;
    BACKUP|FAULT)
        log "${STATE} 강등 → systemctl stop ${UNIT}"
        systemctl stop "${UNIT}" 2>>"$LOG" || log "FAIL: systemctl stop ${UNIT}"
        ;;
    STOP)
        log "keepalived 자체 종료 — ${UNIT} 상태 유지"
        ;;
    *)
        log "알 수 없는 상태 — 무시"
        ;;
esac
exit 0
