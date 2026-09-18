#!/usr/bin/env bash
# nat-netns.sh — 계측기 워커 호스트에 NAT 뒤 단말 망(network namespace)을 만든다 (test_instrument.md §3.1 `nat` 풀, ue_nat_traversal.md 검증).
#   워커의 NAT 풀은 UE 스택 소켓을 이 netns 안에서 만들고(setns), 대상(CSP/CMP)은 호스트 주소로 변환된 소스(NAPT)만 본다 —
#   등록 바인딩 received/rport·미디어 목적지 latch·RTP 포트 변환이 시험 대상이다. 대상 호스트에는 아무것도 하지 않는다.
#
#   sudo nat-netns.sh create <name> [--cidr 10.200.1.0/24] [--out <iface>] [--symmetric]   # netns + veth + 기본 경로 + MASQUERADE
#   sudo nat-netns.sh delete <name>
#   nat-netns.sh status <name>
#   --symmetric : 포트를 무작위로 바꾸는 NAT(iptables MASQUERADE --random-fully) — 대칭형 NAT 모사(기본은 포트 보존 시도)
#   netns 안 단말 주소 = <cidr 의 .2>, 호스트 쪽 veth = <.1>. 풀 정의: nat: { netns: <name>, local_ip: <.2> }
#   워커 실행 파일에는 CAP_SYS_ADMIN 이 필요하다: sudo cims-priv setcap-sys-admin <…/bin/cims-tester-worker> (agent 가 설치·기동마다 건다).
set -euo pipefail
cmd="${1:-}"; name="${2:-}"
[[ -n "$cmd" && -n "$name" ]] || { echo "usage: $0 create|delete|status <name> [--cidr A.B.C.0/24] [--out <iface>] [--symmetric]" >&2; exit 1; }
[[ "$name" =~ ^[a-zA-Z0-9_-]{1,15}$ ]] || { echo "bad netns name: $name" >&2; exit 1; }
cidr="10.200.1.0/24"; out=""; symmetric=0
shift 2 || true
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cidr) cidr="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --symmetric) symmetric=1; shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
base="${cidr%.*}"; prefix="${cidr##*/}"
host_ip="${base}.1"; ue_ip="${base}.2"
veth_h="v-${name}"; veth_n="ve-${name}"
[[ ${#veth_h} -le 15 && ${#veth_n} -le 15 ]] || { echo "netns name too long for veth (${veth_h})" >&2; exit 1; }
[[ -z "$out" ]] && out="$(ip -o route show default | awk '{print $5; exit}')"
case "$cmd" in
  create)
    [[ $EUID -eq 0 ]] || { echo "root required" >&2; exit 2; }
    ip netns list | grep -qw "$name" || ip netns add "$name"
    if ! ip link show "$veth_h" >/dev/null 2>&1; then
      ip link add "$veth_h" type veth peer name "$veth_n"
      ip link set "$veth_n" netns "$name"
    fi
    ip addr replace "${host_ip}/${prefix}" dev "$veth_h"; ip link set "$veth_h" up
    ip -n "$name" addr replace "${ue_ip}/${prefix}" dev "$veth_n"
    ip -n "$name" link set lo up; ip -n "$name" link set "$veth_n" up
    ip -n "$name" route replace default via "$host_ip"
    sysctl -qw net.ipv4.ip_forward=1
    rule=(-s "$cidr" -o "$out" -j MASQUERADE); [[ $symmetric -eq 1 ]] && rule+=(--random-fully)
    iptables -t nat -C POSTROUTING "${rule[@]}" 2>/dev/null || iptables -t nat -A POSTROUTING "${rule[@]}"
    iptables -C FORWARD -i "$veth_h" -o "$out" -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$veth_h" -o "$out" -j ACCEPT
    iptables -C FORWARD -i "$out" -o "$veth_h" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$out" -o "$veth_h" -m state --state RELATED,ESTABLISHED -j ACCEPT
    echo "netns=$name ue_ip=$ue_ip gw=$host_ip out=$out symmetric=$symmetric — 풀: nat: { netns: $name, local_ip: $ue_ip }"
    ;;
  delete)
    [[ $EUID -eq 0 ]] || { echo "root required" >&2; exit 2; }
    for sym in 0 1; do
      rule=(-s "$cidr" -o "$out" -j MASQUERADE); [[ $sym -eq 1 ]] && rule+=(--random-fully)
      iptables -t nat -D POSTROUTING "${rule[@]}" 2>/dev/null || true
    done
    iptables -D FORWARD -i "$veth_h" -o "$out" -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -i "$out" -o "$veth_h" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    ip link del "$veth_h" 2>/dev/null || true
    ip netns del "$name" 2>/dev/null || true
    echo "deleted netns=$name"
    ;;
  status)
    ip netns list | grep -qw "$name" || { echo "netns $name: absent"; exit 3; }
    echo "netns $name:"; ip -n "$name" -brief addr; ip -n "$name" route
    iptables -t nat -S POSTROUTING 2>/dev/null | grep -- "-s $cidr" || echo "(no MASQUERADE rule for $cidr)"
    ;;
  *) echo "unknown command $cmd" >&2; exit 1 ;;
esac
