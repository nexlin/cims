#!/usr/bin/env python3
# CMP PTT 영상 leg 별 PT 재작성 스모크 테스트 — user_video_pt (cmp_media_api.md §7.4)
# ADD(floor on) → JOIN A(video pt 99)·B(video pt 97)·C(video pt 미지정) → A Request → GRANTED
# → A 영상(PT 99, marker 섞음) → B 는 PT 97·marker 보존 / C 는 원본 PT(재작성 없음) / A 자신은 미수신
# → A Release → B Request → B 영상(PT 97) → A 는 PT 99
# 사용법: python3 tests/cmp_smoke_video_pt.py [CMP_IP]  (CMP 9000 라이브 대상, 그룹 grp-smoketest-videopt 생성·정리)
#   CMP_IP 기본 127.0.0.1 (환경변수 CMP_IP·CMP_PORT 도 인식). CMP 와 같은 호스트에서 실행 전제.
import json, os, socket, struct, sys, time

CMP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CMP_IP", "127.0.0.1")
CMP = (CMP_IP, int(os.environ.get("CMP_PORT", "9000")))
GROUP = "grp-smoketest-videopt"
A_ID, B_ID, C_ID = "+82500000001", "+82500000002", "+82500000003"
A_PT, B_PT, C_SEND_PT = 99, 97, 99       # A=Android MediaCodec, B=서버 offer(97) echo, C=PT 미지정
MY_IP = CMP_IP

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctrl.settimeout(3.0)
_trans = [7800300]

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
        body += b"\0" * ((-(2 + len(val))) % 4)
    words = (12 + len(body)) // 4 - 1
    return bytes([0x80 | subtype, 204]) + struct.pack("!H", words) + struct.pack("!I", ssrc) + b"MCPT" + body

def recv_floor(sock, want, timeout=2.0):
    got = []
    sock.settimeout(timeout)
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if len(data) >= 12 and data[1] == 204 and data[8:12] == b"MCPT":
                got.append(data[0] & 0x1F)
                if got[-1] == want:
                    return got
    except socket.timeout:
        return got

def vrtp(seq, pt, ssrc, marker):
    return struct.pack("!BBHII", 0x80, (0x80 if marker else 0) | pt, seq & 0xFFFF, seq * 3000, ssrc) + bytes(64)

def drain(sock):
    """수신 영상 RTP 의 (pt, marker) 목록"""
    out = []
    sock.settimeout(0.5)
    try:
        while True:
            d, _ = sock.recvfrom(4096)
            if len(d) >= 12 and (d[0] >> 6) == 2:
                out.append((d[1] & 0x7F, d[1] >> 7))
    except socket.timeout:
        return out

def send_video(sock, dst_port, pt, ssrc, n=20):
    for i in range(n):
        sock.sendto(vrtp(i + 1, pt, ssrc, marker=(i % 4 == 3)), (MY_IP, dst_port))
        time.sleep(0.01)

# 1) ADD — floor on, prearranged
r = req("PTT_GROUP_ADD", {"group_id": GROUP, "group_type": "prearranged", "initiator_id": A_ID,
                          "members": f"{A_ID}:5:participant,{B_ID}:5:participant,{C_ID}:5:participant",
                          "subid": "1"})
pl = r.get("payload", {})
mp, fport = pl.get("member_ports", {}), pl.get("floor_port")
assert fport, f"floor_port missing: {r}"
vport = {sid: mp[sid]["video_port"] for sid in (A_ID, B_ID, C_ID)}
print(f"ADD: floor_port={fport} video_ports={vport}")

# 2) JOIN — 멤버별 audio/floor/video 소켓, A·B 는 영상 PT 선언, C 는 생략(재작성 없음)
socks = {}
for sid, vpt in ((A_ID, A_PT), (B_ID, B_PT), (C_ID, 0)):
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(("0.0.0.0", 0))
    f = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); f.bind(("0.0.0.0", 0))
    v = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); v.bind(("0.0.0.0", 0))
    p = {"group_id": GROUP, "session_id": sid, "user_ip": MY_IP, "user_port": a.getsockname()[1],
         "user_floor_port": f.getsockname()[1], "user_video_port": v.getsockname()[1], "role": "participant"}
    if vpt: p["user_video_pt"] = vpt
    r = req("PTT_JOIN", p)
    assert r["hdr"].get("status") == "OK", f"JOIN {sid} failed: {r}"
    socks[sid] = (a, f, v)
print("JOIN A/B/C: OK")

# 3) A 발언 — 영상 PT 99
socks[A_ID][1].sendto(floor_msg(0, 0xAAAA0001, [(0, bytes([5, 0]))]), (MY_IP, fport))
granted_a = 1 in recv_floor(socks[A_ID][1], 1)
send_video(socks[A_ID][2], vport[A_ID], A_PT, 0xAAAA0001)
b_rx, c_rx, a_rx = drain(socks[B_ID][2]), drain(socks[C_ID][2]), drain(socks[A_ID][2])
b_ok = len(b_rx) >= 18 and all(pt == B_PT for pt, _ in b_rx) and sum(m for _, m in b_rx) == 5
c_ok = len(c_rx) >= 18 and all(pt == A_PT for pt, _ in c_rx)
print(f"A talks(PT {A_PT}): B rx={len(b_rx)} pts={sorted({p for p, _ in b_rx})} markers={sum(m for _, m in b_rx)} "
      f"(want PT {B_PT}, 5 markers) | C rx={len(c_rx)} pts={sorted({p for p, _ in c_rx})} (want PT {A_PT}) "
      f"| A self rx={len(a_rx)} (want 0)")

# 4) A Release → B 발언 — 영상 PT 97 → A 는 PT 99
socks[A_ID][1].sendto(floor_msg(4, 0xAAAA0001), (MY_IP, fport))
recv_floor(socks[A_ID][1], 5)
socks[B_ID][1].sendto(floor_msg(0, 0xBBBB0002, [(0, bytes([5, 0]))]), (MY_IP, fport))
granted_b = 1 in recv_floor(socks[B_ID][1], 1)
send_video(socks[B_ID][2], vport[B_ID], B_PT, 0xBBBB0002)
a_rx2 = drain(socks[A_ID][2])
a_ok = len(a_rx2) >= 18 and all(pt == A_PT for pt, _ in a_rx2)
print(f"B talks(PT {B_PT}): A rx={len(a_rx2)} pts={sorted({p for p, _ in a_rx2})} (want PT {A_PT})")
socks[B_ID][1].sendto(floor_msg(4, 0xBBBB0002), (MY_IP, fport))

req("PTT_GROUP_REMOVE", {"group_id": GROUP})
ok = granted_a and granted_b and b_ok and c_ok and not a_rx and a_ok
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
