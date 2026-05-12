#!/bin/bash
# notify_csc.sh — keepalived state transition hook for CSC.
# Invoked by keepalived with args: $1=TYPE (INSTANCE/GROUP), $2=NAME, $3=STATE, $4=PRIORITY.
# Phase 1.B: log only. Phase 1.F will add service start/stop here.

TYPE="$1"
NAME="$2"
STATE="$3"
PRIO="$4"

LOG="${HA_LOG_DIR:-/var/log/cims-ha}/notify_csc.log"
mkdir -p "$(dirname "$LOG")" 2>/dev/null

echo "$(date -Iseconds) ${TYPE} ${NAME} -> ${STATE} (prio=${PRIO})" >> "$LOG"

# Phase 1.F TODO:
#   MASTER  -> systemctl start  cims-csc
#   BACKUP  -> systemctl stop   cims-csc   (cold-spare mode, default)
#   FAULT   -> systemctl stop   cims-csc + alert
exit 0
