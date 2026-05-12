#!/bin/bash
# check_csc.sh — keepalived health probe for CSC (mgmt-server)
# Returns 0 if CSC admin port is bound, 1 otherwise.
# Invoked by keepalived vrrp_script (interval=2, timeout=3).

PORT="${CSC_ADMIN_PORT:-4420}"

ss -lnt "sport = :${PORT}" 2>/dev/null | awk 'NR>1 {found=1} END {exit found?0:1}'
