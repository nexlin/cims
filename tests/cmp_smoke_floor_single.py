#!/usr/bin/env python3
# CMP floor-on(싱글) 회귀 스모크 테스트 — TS 24.380 floor 절차
# ADD(floor on) → JOIN×2(+floor 소켓) → A Request → GRANTED(A)/TAKEN(B)
# → A RTP 중계 O / B RTP 중계 X → A Release → IDLE
# 사용법: python3 tests/cmp_smoke_floor_single.py [CMP_IP]  (CMP 9000 라이브 대상, 그룹 grp-smoketest-flooron 생성·정리)
#   CMP_IP 기본 127.0.0.1 — CMP 가 서비스 IP 에만 bind 하면 그 IP 를 넘긴다 (환경변수 CMP_IP 도 인식).
#   CMP 와 같은 호스트에서 실행 전제 — 멤버 수신 주소(MY_IP)도 이 주소로 광고된다.
import json, os, socket, struct, sys, time

CMP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CMP_IP", "127.0.0.1")
CMP = (CMP_IP, 9000)
GROUP = "grp-smoketest-flooron"
A_ID, B_ID = "+82500000001", "+82500000002"
MY_IP = CMP_IP

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctrl.settimeout(3.0)
_trans = [7800100]

def req(cmd, payload):
    _trans[0] += 1
    msg = {"hdr": {"cmd": cmd, "node": "smoketest", "service": "mcptt",
                   "sesid": f"{GROUP}::smoke::{_trans[0]}", "trans_id": _trans[0],
                   "type": "request", "ver": 2}, "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), CMP)
    data, _ = ctrl.recvfrom(8192)
    return json.loads(data.decode())

def floor_msg(subtype, ssrc, fields=()):
    body = b""
    for fid, val in fields:
        body += bytes([fid, len(val)]) + val
        pad = (-(2 + len(val))) % 4
        body += b"\0" * pad
    total = 12 + len(body)
    words = total // 4 - 1
    return (bytes([0x80 | subtype, 204]) + struct.pack("!H", words) +
            struct.pack("!I", ssrc) + b"MCPT" + body)

def recv_floor(sock, want, timeout=2.0):
    """want subtype 수신까지 드레인. (subtype, raw) 리스트 반환"""
    got = []
    sock.settimeout(timeout)
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if len(data) >= 12 and data[1] == 204 and data[8:12] == b"MCPT":
                st = data[0] & 0x1F
                got.append(st)
                if st == want:
                    return got
    except socket.timeout:
        return got

# 1) ADD — floor on (기본), prearranged
r = req("PTT_GROUP_ADD", {
    "group_id": GROUP, "group_type": "prearranged",
    "initiator_id": A_ID,
    "members": f"{A_ID}:5:participant,{B_ID}:5:participant", "subid": "1"})
pl = r.get("payload", {})
mp = pl.get("member_ports", {})
fport = pl.get("floor_port")
print(f"ADD: floor_port={fport} member_ports A={mp.get(A_ID,{}).get('port')} B={mp.get(B_ID,{}).get('port')}")
assert fport, "floor_port missing on floor-on group"
a_port, b_port = mp[A_ID]["port"], mp[B_ID]["port"]

# 2) 멤버 RTP + floor 소켓, JOIN
sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sa.bind(("0.0.0.0", 0)); sa.settimeout(2.0)
sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sb.bind(("0.0.0.0", 0)); sb.settimeout(2.0)
fa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); fa.bind(("0.0.0.0", 0))
fb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); fb.bind(("0.0.0.0", 0))
for sid, s, f in ((A_ID, sa, fa), (B_ID, sb, fb)):
    r = req("PTT_JOIN", {"group_id": GROUP, "session_id": sid,
                         "user_ip": MY_IP, "user_port": s.getsockname()[1],
                         "user_floor_port": f.getsockname()[1], "role": "participant"})
    assert r["hdr"].get("status") == "OK", f"JOIN {sid} failed: {r}"
print("JOIN A/B: OK")

# 3) A Floor Request (subtype 0, priority TLV=prio 1옥텟+예약 1옥텟)
fa.sendto(floor_msg(0, 0xAAAA0001, [(0, bytes([5, 0]))]), (MY_IP, fport))
a_floor = recv_floor(fa, 1)   # 1=GRANTED
b_floor = recv_floor(fb, 2)   # 2=TAKEN
print(f"A floor msgs={a_floor} (want 1=GRANTED)  B floor msgs={b_floor} (want 2=TAKEN)")
granted = 1 in a_floor
taken = 2 in b_floor

# 4) 발언자 A 의 RTP → B 중계 / 비발언자 B 의 RTP → A 미중계
def rtp(seq, ssrc):
    return struct.pack("!BBHII", 0x80, 96, seq & 0xFFFF, seq * 320, ssrc) + bytes(32)
for i in range(20):
    sa.sendto(rtp(i + 1, 0xAAAA0001), (MY_IP, a_port))
    sb.sendto(rtp(i + 1, 0xBBBB0002), (MY_IP, b_port))
    time.sleep(0.02)
def drain(sock):
    n = 0
    try:
        while True:
            d, _ = sock.recvfrom(2048)
            if len(d) >= 12 and (d[0] >> 6) == 2 and d[1] != 204:
                n += 1
    except socket.timeout:
        return n
b_got, a_got = drain(sb), drain(sa)
print(f"talker A→B relay: {b_got}/20 (want>=18)  |  non-talker B→A relay: {a_got} (want 0)")

# 5) A Release (subtype 4) → 양쪽 IDLE(5)
fa.sendto(floor_msg(4, 0xAAAA0001), (MY_IP, fport))
a_idle = 5 in recv_floor(fa, 5)
b_idle = 5 in recv_floor(fb, 5)
print(f"Release → IDLE: A={a_idle} B={b_idle}")

req("PTT_GROUP_REMOVE", {"group_id": GROUP})
ok = granted and taken and b_got >= 18 and a_got == 0 and a_idle and b_idle
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
