#!/usr/bin/env bash
# verify/scripts/ha-netns-down.sh — HA NetNS 환경 정리
#
# ha-netns-up.sh 가 만든 ns 4개 + bridge 3개 + 호스트측 dangling veth 제거.
# idempotent — 없으면 조용히 skip.
set -euo pipefail

[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

NODES=(ctrl-a ctrl-b media-a media-b)
BRIDGES=(br-cims-mgmt br-cims-svc br-cims-int)

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

log "=== HA NetNS 환경 정리 시작 ==="

# 1. namespace 제거 (안의 veth 자동 해제)
for ns in "${NODES[@]}"; do
    if ip netns list | grep -q "^$ns\b"; then
        ip netns delete "$ns"
        log "ns deleted: $ns"
    fi
done

# 2. 호스트측 dangling veth (v-<ns>-<nic>) 제거
for ns in "${NODES[@]}"; do
    for nic in mgmt svc int; do
        v="v-${ns}-${nic}"
        if ip link show "$v" >/dev/null 2>&1; then
            ip link delete "$v"
            log "veth deleted: $v"
        fi
    done
done

# 3. bridge 제거
for br in "${BRIDGES[@]}"; do
    if ip link show "$br" >/dev/null 2>&1; then
        # bridge 에 아직 붙어있는 port 있으면 강제 해제
        for port in $(bridge link show 2>/dev/null | awk -v br="$br" '$0 ~ "master "br" " {print $2}'); do
            ip link delete "$port" 2>/dev/null || true
        done
        ip link set "$br" down
        ip link delete "$br"
        log "bridge deleted: $br"
    fi
done

log "=== 정리 완료 ==="
