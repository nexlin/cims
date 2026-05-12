#!/bin/bash
# notify_psp.sh — keepalived state transition hook for PSP.
# Invoked by keepalived with args: $1=TYPE, $2=NAME, $3=STATE, $4=PRIORITY.
# Phase 1.B: log only. Phase 1.F will add service start/stop + Redis register restore here.

TYPE="$1"
NAME="$2"
STATE="$3"
PRIO="$4"

LOG="${HA_LOG_DIR:-/var/log/cims-ha}/notify_psp.log"
mkdir -p "$(dirname "$LOG")" 2>/dev/null

echo "$(date -Iseconds) ${TYPE} ${NAME} -> ${STATE} (prio=${PRIO})" >> "$LOG"

# Phase 1.F TODO:
#   MASTER  -> systemctl start  cims-psp + trigger Redis register replay (1.D-1)
#   BACKUP  -> systemctl stop   cims-psp
#   FAULT   -> systemctl stop   cims-psp + alert
exit 0
