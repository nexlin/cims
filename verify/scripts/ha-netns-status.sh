#!/usr/bin/env bash
# verify/scripts/ha-netns-status.sh — 환경 상태 + 노드간 연결성 점검
#
# 출력:
#   1. bridge 목록 + 호스트 IP
#   2. 각 ns 의 IP 매트릭스
#   3. ns 간 ping 매트릭스 (대각선 skip)
#   4. VRRP multicast (224.0.0.18) reachability (브로드캐스트 ping)
set -euo pipefail

[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

NODES=(ctrl-a ctrl-b media-a media-b)
BRIDGES=(br-cims-mgmt br-cims-svc br-cims-int)

declare -A MGMT=(
    [ctrl-a]=10.0.0.11 [ctrl-b]=10.0.0.12
    [media-a]=10.0.0.21 [media-b]=10.0.0.22
)
declare -A SVC=(
    [ctrl-a]=10.0.1.11 [ctrl-b]=10.0.1.12
    [media-a]=10.0.1.21 [media-b]=10.0.1.22
)
declare -A INT=(
    [ctrl-a]=10.0.2.11 [ctrl-b]=10.0.2.12
    [media-a]=10.0.2.21 [media-b]=10.0.2.22
)

# ─── 1. Bridges ───────────────────────────────────────────────────
echo "== Bridges =="
for br in "${BRIDGES[@]}"; do
    if ip link show "$br" >/dev/null 2>&1; then
        ip -br -4 addr show dev "$br" | awk '{printf "  %-18s up=%s ip=%s\n", $1, $2, $3}'
        # snoop 상태
        snoop=$(cat /sys/devices/virtual/net/$br/bridge/multicast_snooping 2>/dev/null || echo "?")
        ports=$(bridge link show 2>/dev/null | awk -v br="$br" '$0 ~ "master "br" " {print $2}' | tr '\n' ' ')
        printf "      snoop=%s  ports=%s\n" "$snoop" "${ports:-(none)}"
    else
        printf "  %-18s MISSING\n" "$br"
    fi
done
echo

# ─── 2. Namespaces ────────────────────────────────────────────────
echo "== Namespaces =="
for ns in "${NODES[@]}"; do
    if ! ip netns list | grep -q "^$ns\b"; then
        printf "  %-8s MISSING\n" "$ns"
        continue
    fi
    printf "  %-8s\n" "$ns"
    ip netns exec "$ns" ip -br -4 addr show 2>/dev/null \
        | awk '$1 != "lo" {printf "    %-6s %-6s %s\n", $1, $2, $3}'
done
echo

# ─── 3. Ping matrix ───────────────────────────────────────────────
echo "== Connectivity (ping -c 1 -W 1) =="

declare -A IP_OF=()
for ns in "${NODES[@]}"; do
    IP_OF["$ns:mgmt"]=${MGMT[$ns]}
    IP_OF["$ns:svc"]=${SVC[$ns]}
    IP_OF["$ns:int"]=${INT[$ns]}
done

run_ping() {
    local from=$1 target_ip=$2
    if ip netns exec "$from" ping -c 1 -W 1 "$target_ip" >/dev/null 2>&1; then
        echo "✓"
    else
        echo "✗"
    fi
}

for bridge in mgmt svc int; do
    echo "  -- $bridge bridge --"
    printf "    %-9s" ""
    for to in "${NODES[@]}"; do printf " %-9s" "$to"; done
    echo
    for from in "${NODES[@]}"; do
        printf "    %-9s" "$from"
        for to in "${NODES[@]}"; do
            if [[ $from == $to ]]; then
                printf " %-9s" "—"
            else
                result=$(run_ping "$from" "${IP_OF["$to:$bridge"]}")
                printf " %-9s" "$result"
            fi
        done
        echo
    done
done
echo

# ─── 4. VRRP multicast reachability ───────────────────────────────
echo "== VRRP multicast (224.0.0.18) =="
for bridge in mgmt svc int; do
    nic="$bridge"
    sender=ctrl-a
    listener=ctrl-b
    if ! ip netns list | grep -q "^$sender\b" || ! ip netns list | grep -q "^$listener\b"; then
        printf "  %-5s  SKIP (ns 없음)\n" "$bridge"
        continue
    fi
    # listener 에서 짧게 tcpdump → sender 가 multicast ping → 수신 여부
    tmp=$(mktemp)
    ip netns exec "$listener" timeout 2 tcpdump -ni "$nic" 'host 224.0.0.18 or igmp' -c 1 \
        >"$tmp" 2>&1 &
    listener_pid=$!
    sleep 0.3
    ip netns exec "$sender" ping -c 1 -W 1 -I "$nic" 224.0.0.18 >/dev/null 2>&1 || true
    wait $listener_pid 2>/dev/null || true
    if grep -q '224\.0\.0\.18' "$tmp" 2>/dev/null; then
        printf "  %-5s  ✓ (ctrl-a → ctrl-b multicast reach)\n" "$bridge"
    else
        printf "  %-5s  ✗ (multicast unreach — VRRP fail-over 작동 안할 수 있음)\n" "$bridge"
    fi
    rm -f "$tmp"
done
