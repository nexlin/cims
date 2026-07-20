#!/bin/bash
# nat_netns_sim.sh — cspsim 을 사설 네임스페이스(10.99.0.2) 뒤 NAT 로 보내는 풀 NAT 모사.
#   UE 는 SDP/Via 에 사설 10.99.0.2 를 선언하고, CSP/CMP 는 SNAT 된 10.99.0.1 을 관측 →
#   access_services media_nat_mode=auto 판정 + latch_ip_guard(strict) + CMP 목적지 latch 가
#   실호(SIP E2E)에서 발동한다. 절차는 docs/VERIFICATION_MANUAL.md "NAT 호시험" 참조.
#
# 사용:
#   sudo bash scripts/nat_netns_sim.sh setup
#   sudo ip netns exec uenat sudo -u cims bash -c \
#     'cd /home/cims/work/cims && ./cims.sh sim -mode ptt -scenario group_call -group g001 -count 4 -call_duration 10'
#   sudo bash scripts/nat_netns_sim.sh teardown
set -e
NS=uenat
HOST_IP=${HOST_IP:-121.161.164.48}   # CSP/CMP bind IP
PRIV_NET=10.99.0.0/24
PRIV_HOST=10.99.0.1
PRIV_UE=10.99.0.2

setup() {
    ip netns add $NS 2>/dev/null || true
    ip link add veth-ue type veth peer name veth-host 2>/dev/null || true
    ip link set veth-ue netns $NS
    ip addr add $PRIV_HOST/24 dev veth-host 2>/dev/null || true
    ip link set veth-host up
    ip netns exec $NS ip addr add $PRIV_UE/24 dev veth-ue
    ip netns exec $NS ip link set veth-ue up
    ip netns exec $NS ip link set lo up
    ip netns exec $NS ip route add default via $PRIV_HOST
    # ① 사설망 → 호스트 로컬 서비스(CSP/CMP): nat INPUT 에서 SNAT — 포트변환 NAT 라우터 모사
    iptables -t nat -A INPUT -s $PRIV_UE -d $HOST_IP -j SNAT --to-source $PRIV_HOST
    # ② 사설망 → 외부(DB 등): 포워딩 + MASQUERADE (cspsim -db 가입자 로드 경로)
    sysctl -qw net.ipv4.ip_forward=1
    iptables -t nat -A POSTROUTING -s $PRIV_NET ! -d $PRIV_NET -j MASQUERADE
    iptables -I FORWARD 1 -s $PRIV_NET -j ACCEPT
    iptables -I FORWARD 1 -d $PRIV_NET -j ACCEPT
    echo "setup 완료 — UE=$PRIV_UE (호스트 로컬 관측 소스는 $PRIV_HOST 로 SNAT)"
}

teardown() {
    iptables -t nat -D INPUT -s $PRIV_UE -d $HOST_IP -j SNAT --to-source $PRIV_HOST 2>/dev/null || true
    iptables -t nat -D POSTROUTING -s $PRIV_NET ! -d $PRIV_NET -j MASQUERADE 2>/dev/null || true
    iptables -D FORWARD -s $PRIV_NET -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -d $PRIV_NET -j ACCEPT 2>/dev/null || true
    ip link del veth-host 2>/dev/null || true
    ip netns del $NS 2>/dev/null || true
    echo "teardown 완료"
}

case "$1" in
    setup) setup ;;
    teardown) teardown ;;
    *) echo "usage: sudo bash $0 setup|teardown"; exit 2 ;;
esac
