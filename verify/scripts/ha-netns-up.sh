#!/usr/bin/env bash
# verify/scripts/ha-netns-up.sh — 4-node HA 검증용 NetNS 환경 구성
#
# 토폴로지:
#   ctrl-a, ctrl-b   = A/S Control-Server (CSC/Console/CSP/PSP/ISP/CWRTC/CSPSIM/Phone)
#   media-a, media-b = All-Active Media-Server (CMP/PMP/IMP)
#
# 3개 L2 도메인 (linux bridge):
#   br-cims-mgmt (10.0.0.0/24)  — 관리 (CSC admin/heartbeat/agent enrollment)
#   br-cims-svc  (10.0.1.0/24)  — 서비스 (SIP signaling, RTP, 단말/외부 연동)
#   br-cims-int  (10.0.2.0/24)  — 내부 모듈간 연동 (CSP↔CMP, Redis 등)
#
# 호스트는 각 bridge 의 .1 을 차지 (ns 외부 도구 접근용, 게이트웨이 아님).
#
# 사용:
#   sudo ./verify/scripts/ha-netns-up.sh         # 환경 구축 (idempotent)
#   sudo ./verify/scripts/ha-netns-down.sh       # 정리
#   sudo ./verify/scripts/ha-netns-status.sh     # 상태 확인 + ping 매트릭스
#
# 한 ns 진입:  sudo ip netns exec ctrl-a bash
set -euo pipefail

[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

# ─── 노드 정의 ───────────────────────────────────────────────────────
# ns:mgmt_ip:svc_ip:int_ip
NODES=(
    "ctrl-a:10.0.0.11:10.0.1.11:10.0.2.11"
    "ctrl-b:10.0.0.12:10.0.1.12:10.0.2.12"
    "media-a:10.0.0.21:10.0.1.21:10.0.2.21"
    "media-b:10.0.0.22:10.0.1.22:10.0.2.22"
)

# bridge:host_ip
BRIDGES=(
    "br-cims-mgmt:10.0.0.1"
    "br-cims-svc:10.0.1.1"
    "br-cims-int:10.0.2.1"
)

# bridge → ns 내부 NIC 명
declare -A BR_TO_NIC=(
    [br-cims-mgmt]=mgmt
    [br-cims-svc]=svc
    [br-cims-int]=int
)

# ─── helpers ─────────────────────────────────────────────────────────
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

ensure_bridge() {
    local br=$1 host_ip=$2
    if ! ip link show "$br" >/dev/null 2>&1; then
        ip link add name "$br" type bridge
        log "bridge created: $br"
    fi
    ip link set "$br" up
    if ! ip -4 addr show dev "$br" | grep -q "inet $host_ip/"; then
        ip addr add "$host_ip/24" dev "$br"
        log "host ip $host_ip/24 added on $br"
    fi
    # VRRP multicast (224.0.0.18) 가 bridge 를 통과하도록 snooping 비활성
    local snoop=/sys/devices/virtual/net/$br/bridge/multicast_snooping
    [[ -w $snoop ]] && echo 0 > "$snoop"
}

gen_mac() {
    # 02:xx:xx:xx:xx:xx — locally administered, unicast. veth peer 가 NIC
    # 이름으로부터 deterministic MAC 을 받아 ns 간 충돌나는 것을 방지.
    printf '02:%02x:%02x:%02x:%02x:%02x' \
        $((RANDOM%256)) $((RANDOM%256)) $((RANDOM%256)) \
        $((RANDOM%256)) $((RANDOM%256))
}

attach_nic() {
    local ns=$1 br=$2 ip_addr=$3
    local nic="${BR_TO_NIC[$br]}"
    local host_veth="v-${ns}-${nic}"
    local ns_veth="$nic"

    # 기존 host-side veth 가 있으면 깨끗이 재생성
    if ip link show "$host_veth" >/dev/null 2>&1; then
        ip link delete "$host_veth"
    fi

    ip link add "$host_veth" type veth peer name "$ns_veth"
    ip link set "$ns_veth" netns "$ns"
    ip link set "$host_veth" master "$br"
    ip link set "$host_veth" up

    # ns 안에서 NIC 이름이 ns 별로 같아 (mgmt/svc/int) kernel 이 같은
    # MAC 을 부여할 수 있다 → 명시적으로 unique MAC 할당.
    ip netns exec "$ns" ip link set "$ns_veth" address "$(gen_mac)"
    ip netns exec "$ns" ip link set "$ns_veth" up
    ip netns exec "$ns" ip addr add "$ip_addr/24" dev "$ns_veth"
}

ensure_ns() {
    local ns=$1
    if ! ip netns list | grep -q "^$ns\b"; then
        ip netns add "$ns"
        log "ns created: $ns"
    fi
    ip netns exec "$ns" ip link set lo up

    # ns 내부 sysctl — 멀티캐스트 + ARP
    ip netns exec "$ns" sysctl -qw net.ipv4.conf.all.arp_announce=2 || true
    ip netns exec "$ns" sysctl -qw net.ipv4.conf.all.arp_ignore=1   || true
    ip netns exec "$ns" sysctl -qw net.ipv4.ip_forward=0            || true
}

# ─── 구축 ───────────────────────────────────────────────────────────
log "=== HA NetNS 환경 구축 시작 ==="

for spec in "${BRIDGES[@]}"; do
    IFS=: read -r br host_ip <<<"$spec"
    ensure_bridge "$br" "$host_ip"
done

for spec in "${NODES[@]}"; do
    IFS=: read -r ns mgmt_ip svc_ip int_ip <<<"$spec"
    ensure_ns "$ns"
    attach_nic "$ns" br-cims-mgmt "$mgmt_ip"
    attach_nic "$ns" br-cims-svc  "$svc_ip"
    attach_nic "$ns" br-cims-int  "$int_ip"
    log "ns ready: $ns  mgmt=$mgmt_ip  svc=$svc_ip  int=$int_ip"
done

# ─── 요약 ───────────────────────────────────────────────────────────
echo
log "=== 구축 완료 ==="
echo
echo "Bridges (host side):"
for spec in "${BRIDGES[@]}"; do
    br="${spec%%:*}"
    ip -br -4 addr show dev "$br" | awk '{printf "  %-18s %s\n", $1, $3}'
done
echo
echo "Namespaces:"
for spec in "${NODES[@]}"; do
    ns="${spec%%:*}"
    echo "  $ns:"
    ip netns exec "$ns" ip -br -4 addr show | awk '$1 != "lo" {printf "    %-6s %s\n", $1, $3}'
done

cat <<'TIP'

다음 단계:
  sudo ./verify/scripts/ha-netns-status.sh           # 상태 + ping 매트릭스
  sudo ip netns exec ctrl-a bash                      # ns 진입
  sudo ip netns exec ctrl-a ping -c 1 10.0.0.12      # 노드간 통신 확인

각 ns 안에서 cims_agent / CSP / CMP / Redis 등을 띄우면 됩니다.
권장 VIP (keepalived 가 부여 — 본 스크립트는 IP 할당 안 함):
  CSC=10.0.0.100  CSP=10.0.1.100  PSP=10.0.1.101  ISP=10.0.1.102
TIP
