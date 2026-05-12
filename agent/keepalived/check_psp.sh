#!/bin/bash
# check_psp.sh — keepalived health probe for PSP (PTT SIP server)
# Returns 0 if PSP SIP UDP port is bound, 1 otherwise.
# Invoked by keepalived vrrp_script (interval=2, timeout=3).

PORT="${PSP_SIP_PORT:-5060}"
BIND_IP="${PSP_BIND_IP:-}"

if [ -n "$BIND_IP" ]; then
    ss -lnu "src ${BIND_IP}:${PORT}" 2>/dev/null | awk 'NR>1 {found=1} END {exit found?0:1}'
else
    ss -lnu "sport = :${PORT}" 2>/dev/null | awk 'NR>1 {found=1} END {exit found?0:1}'
fi
