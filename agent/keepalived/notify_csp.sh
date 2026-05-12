#!/bin/bash
# notify_csp.sh — keepalived state transition hook for CSP.
# Invoked by keepalived with args: $1=TYPE, $2=NAME, $3=STATE, $4=PRIORITY.
# Phase 1.B: log only. Phase 1.F will add service start/stop + Redis register restore here.

TYPE="$1"
NAME="$2"
STATE="$3"
PRIO="$4"

LOG="${HA_LOG_DIR:-/var/log/cims-ha}/notify_csp.log"
mkdir -p "$(dirname "$LOG")" 2>/dev/null

echo "$(date -Iseconds) ${TYPE} ${NAME} -> ${STATE} (prio=${PRIO})" >> "$LOG"

# Phase 1.F TODO:
#   MASTER  -> systemctl start  cims-csp + trigger Redis register replay (1.D-1)
#   BACKUP  -> systemctl stop   cims-csp
#   FAULT   -> systemctl stop   cims-csp + alert
exit 0
