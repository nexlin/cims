#!/usr/bin/env python3
# CMP floor-off(멀티 1:1) RTP 하향 중계 스모크 테스트
# ADD(private, floor_control=off) → JOIN×2(로컬 소켓) → A→B / B→A RTP 중계 확인 → REMOVE
# 사용법: python3 tests/cmp_smoke_private_flooroff.py [CMP_IP]  (CMP 9000 라이브 대상, 그룹 priv-smoketest-relay 생성·정리)
#   CMP_IP 기본 127.0.0.1 — CMP 가 서비스 IP 에만 bind 하면 그 IP 를 넘긴다 (환경변수 CMP_IP 도 인식).
#   CMP 와 같은 호스트에서 실행 전제 — 멤버 수신 주소(MY_IP)도 이 주소로 광고된다.
import json, os, socket, struct, sys, time

CMP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CMP_IP", "127.0.0.1")
CMP = (CMP_IP, 9000)
GROUP = "priv-smoketest-relay"
A_ID, B_ID = "+82500000001", "+82500000002"
MY_IP = CMP_IP   # 같은 호스트 → 목적지 주소가 소스 선택

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ctrl.settimeout(3.0)
_trans = [7700100]

def req(cmd, payload):
    _trans[0] += 1
    msg = {"hdr": {"cmd": cmd, "node": "smoketest", "service": "mcptt",
                   "sesid": f"{GROUP}::smoke::{_trans[0]}", "trans_id": _trans[0],
                   "type": "request", "ver": 2},
           "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), CMP)
    data, _ = ctrl.recvfrom(8192)
    return json.loads(data.decode())

# 1) ADD — private, floor off
r = req("PTT_GROUP_ADD", {
    "group_id": GROUP, "group_type": "private", "floor_control": "off",
    "initiator_id": A_ID,
    "members": f"{A_ID}:5:participant,{B_ID}:5:participant", "subid": "1"})
pl = r.get("payload", {})
mp = pl.get("member_ports", {})
print("ADD resp:", json.dumps(r)[:300])
assert mp.get(A_ID) and mp.get(B_ID), "member_ports missing"
assert "floor_port" not in pl, f"floor_port advertised on floor-off: {pl.get('floor_port')}"
a_port, b_port = mp[A_ID]["port"], mp[B_ID]["port"]
print(f"member ports: A={a_port} B={b_port} (floor_port 미광고=OK)")

# 2) 로컬 멤버 소켓 + JOIN
sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sa.bind(("0.0.0.0", 0)); sa.settimeout(2.0)
sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sb.bind(("0.0.0.0", 0)); sb.settimeout(2.0)
for sid, sock in ((A_ID, sa), (B_ID, sb)):
    r = req("PTT_JOIN", {"group_id": GROUP, "session_id": sid,
                         "user_ip": MY_IP, "user_port": sock.getsockname()[1],
                         "role": "participant"})
    print(f"JOIN {sid}:", json.dumps(r)[:200])

def rtp(seq, ssrc, pt=96):
    return struct.pack("!BBHII", 0x80, pt, seq & 0xFFFF, seq * 320, ssrc) + bytes(32)

def pump(src_sock, member_port, n=20, ssrc=0x11111111):
    for i in range(n):
        src_sock.sendto(rtp(i + 1, ssrc), (MY_IP, member_port))
        time.sleep(0.02)

def drain(sock):
    got = 0
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if len(data) >= 12 and (data[0] >> 6) == 2:
                got += 1
    except socket.timeout:
        pass
    return got

# 3) A→B 하향 (이어서 B→A — 멀티=양방향)
pump(sa, a_port, ssrc=0xAAAA0001)
pump(sb, b_port, ssrc=0xBBBB0002)
b_got = drain(sb)
a_got = drain(sa)
print(f"A→B relay: B 수신 {b_got}/20  |  B→A relay: A 수신 {a_got}/20")

# 4) 정리
r = req("PTT_GROUP_REMOVE", {"group_id": GROUP})
print("REMOVE:", json.dumps(r)[:150])

ok = b_got >= 18 and a_got >= 18
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
