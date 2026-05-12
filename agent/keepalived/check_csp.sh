#!/bin/bash
# check_csp.sh — keepalived health probe for CSP (VoLTE SIP server)
# Returns 0 if CSP SIP UDP port is bound, 1 otherwise.
# Invoked by keepalived vrrp_script (interval=2, timeout=3).

PORT="${CSP_SIP_PORT:-5060}"

ss -lnu "sport = :${PORT}" 2>/dev/null | awk 'NR>1 {found=1} END {exit found?0:1}'
