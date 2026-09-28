#!/usr/bin/env python3
# CMP 일제 통화(broadcast group call) 스모크 테스트 — mcptt_broadcast_group_call.md §4.2 M1~M3 / cmp_media_api.md
#   ADD(broadcast:1, initiator A, t4_inactivity 2) → JOIN A/B/C → 같은 sesid 재ADD(initiator B, broadcast 0)
#   → B Request = Deny #5 (개시자 고정, R7) → A Request = Granted, B·C Taken(B-bit·Permission 0)
#   → A Release = Idle(B-bit) → T4 만료 PTT_FLOOR_INACTIVITY 이벤트 → 다른 sesid 재ADD(broadcast 0) = 새 세션 → B Granted
#   → 전환기: group_type:"broadcast" 를 broadcast=1 로 해석
# 사용법: python3 tests/cmp_smoke_broadcast.py [CMP_IP] [CMP_PORT]
#   **라이브 CMP 에 돌리지 않는다** — 이벤트 회신처가 마지막 요청 소켓이라 이 스크립트가 CSP 의 이벤트를 가로챈다.
#   시험용 CMP 를 따로 띄운다(ServerPort 만 다른 설정, 같은 호스트 — 멤버 수신 주소가 CMP_IP 로 광고된다).
import json, os, socket, struct, sys, time

CMP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CMP_IP", "127.0.0.1")
CMP = (CMP_IP, int(sys.argv[2]) if len(sys.argv) > 2 else int(os.environ.get("CMP_PORT", "9000")))
GROUP = "grp-smoketest-broadcast"
GROUP2 = "grp-smoketest-broadcast-legacy"
A_ID, B_ID, C_ID = "+82500000011", "+82500000012", "+82500000013"
MY_IP = CMP_IP
FI_BROADCAST = 0x4000
FLOOR_GRANT, FLOOR_TAKEN, FLOOR_DENY, FLOOR_IDLE = 1, 2, 3, 5

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctrl.settimeout(3.0)
_trans = [7810100]
events = []

def _ack(ev):
    events.append(ev)
    ctrl.sendto(json.dumps({"hdr": {"ver": 2, "trans_id": ev["hdr"]["trans_id"], "node": "smoketest",
                                    "cmd": ev["hdr"]["cmd"], "type": "response", "status": "OK"}}).encode(), CMP)

def req(cmd, payload, sesid):
    _trans[0] += 1
    msg = {"hdr": {"cmd": cmd, "node": "smoketest", "service": "mcptt", "sesid": sesid,
                   "trans_id": _trans[0], "type": "request", "ver": 2}, "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), CMP)
    ctrl.settimeout(3.0)
    while True:
        data, _ = ctrl.recvfrom(8192)
        r = json.loads(data.decode())
        if r["hdr"].get("type") == "event":
            _ack(r); continue
        return r

def wait_event(name, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(e["hdr"]["cmd"] == name for e in events):
            return True
        ctrl.settimeout(max(0.05, end - time.time()))
        try:
            data, _ = ctrl.recvfrom(8192)
            r = json.loads(data.decode())
            if r["hdr"].get("type") == "event":
                _ack(r)
        except socket.timeout:
            pass
    return any(e["hdr"]["cmd"] == name for e in events)

def floor_msg(subtype, ssrc, fields=()):
    body = b""
    for fid, val in fields:
        body += bytes([fid, len(val)]) + val
        body += b"\0" * ((-(2 + len(val))) % 4)
    words = (12 + len(body)) // 4 - 1
    return bytes([0x80 | subtype, 204]) + struct.pack("!H", words) + struct.pack("!I", ssrc) + b"MCPT" + body

def parse_fields(data):
    out, i = {}, 12
    while i + 2 <= len(data):
        fid, ln = data[i], data[i + 1]
        out[fid] = data[i + 2:i + 2 + ln]
        i += 2 + ln + ((-(2 + ln)) % 4)
    return out

def recv_floor(sock, want, timeout=2.0):
    """want subtype 수신까지 드레인 → 그 메시지 필드 dict (없으면 None)."""
    sock.settimeout(timeout)
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if len(data) >= 12 and data[1] == 204 and data[8:12] == b"MCPT" and (data[0] & 0x1F) == want:
                return parse_fields(data)
    except socket.timeout:
        return None

def u16(fields, fid):
    v = fields.get(fid) if fields else None
    return struct.unpack("!H", v[:2])[0] if v and len(v) >= 2 else None

def add(group, sesid, **kw):
    p = {"group_id": group, "members": f"{A_ID}:5:participant,{B_ID}:5:participant,{C_ID}:5:participant",
         "subid": "1"}
    p.update(kw)
    return req("PTT_GROUP_ADD", p, sesid)

def join(group, sesid, socks):
    for sid, (s, f) in socks.items():
        r = req("PTT_JOIN", {"group_id": group, "session_id": sid, "user_ip": MY_IP,
                             "user_port": s.getsockname()[1], "user_floor_port": f.getsockname()[1],
                             "role": "participant"}, sesid)
        assert r["hdr"].get("status") == "OK", f"JOIN {sid} failed: {r}"

def mk_socks():
    out = {}
    for sid in (A_ID, B_ID, C_ID):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((MY_IP, 0))
        f = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); f.bind((MY_IP, 0))
        out[sid] = (s, f)
    return out

results = []
def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

S1, S2 = f"{GROUP}::s1", f"{GROUP}::s2"
req("PTT_GROUP_REMOVE", {"group_id": GROUP}, S1)
req("PTT_GROUP_REMOVE", {"group_id": GROUP2}, S1)

# 1) 세션 생성 — broadcast:1, 개시자 A, T4 2초
r = add(GROUP, S1, group_type="prearranged", broadcast=1, initiator_id=A_ID,
        floor_timers={"t4_inactivity": 2})
fport = r.get("payload", {}).get("floor_port")
check("ADD broadcast:1", r["hdr"].get("status") == "OK" and bool(fport), f"floor_port={fport}")
socks = mk_socks()
join(GROUP, S1, socks)
fa, fb, fc = socks[A_ID][1], socks[B_ID][1], socks[C_ID][1]

# 2) 같은 세션의 재ADD 가 개시자·broadcast 를 바꾸지 않는다 (R7)
r = add(GROUP, S1, group_type="prearranged", broadcast=0, initiator_id=B_ID, floor_timers={"t4_inactivity": 2})
check("re-ADD same sesid OK", r["hdr"].get("status") == "OK")

# 3) 비개시자 B 요청 → Deny #5
fb.sendto(floor_msg(0, 0xBBBB0012, [(0, bytes([5, 0]))]), (MY_IP, fport))
d = recv_floor(fb, FLOOR_DENY)
check("non-initiator B → Deny #5", d is not None and u16(d, 2) == 5, f"cause={u16(d, 2)}")

# 4) 개시자 A 요청 → Granted(B-bit), C Taken(B-bit·Permission 0)
fa.sendto(floor_msg(0, 0xAAAA0011, [(0, bytes([5, 0]))]), (MY_IP, fport))
g = recv_floor(fa, FLOOR_GRANT)
t = recv_floor(fc, FLOOR_TAKEN)
check("initiator A → Granted B-bit", g is not None and (u16(g, 13) or 0) & FI_BROADCAST != 0,
      f"indicator={u16(g, 13)}")
check("member C Taken B-bit·Permission 0",
      t is not None and (u16(t, 13) or 0) & FI_BROADCAST != 0 and u16(t, 5) == 0,
      f"indicator={u16(t, 13)} perm={u16(t, 5)}")

# 5) A Release → Idle(B-bit), T4(2초) 만료 → PTT_FLOOR_INACTIVITY
events.clear()
fa.sendto(floor_msg(4, 0xAAAA0011), (MY_IP, fport))
i = recv_floor(fc, FLOOR_IDLE)
check("Release → Idle B-bit", i is not None and (u16(i, 13) or 0) & FI_BROADCAST != 0, f"indicator={u16(i, 13)}")
got = wait_event("PTT_FLOOR_INACTIVITY", 4.5)
ev = next((e for e in events if e["hdr"]["cmd"] == "PTT_FLOOR_INACTIVITY"), None)
check("T4 → PTT_FLOOR_INACTIVITY", got and ev["payload"].get("group_id") == GROUP and ev["hdr"].get("sesid") == S1,
      json.dumps(ev["payload"]) if ev else "")

# 6) 다른 sesid 의 개시 ADD(broadcast 0) = 새 세션 → 일반 그룹 통화, B 요청 Granted
r = add(GROUP, S2, group_type="prearranged", broadcast=0, initiator_id=B_ID)
fb.sendto(floor_msg(0, 0xBBBB0012, [(0, bytes([5, 0]))]), (MY_IP, fport))
g = recv_floor(fb, FLOOR_GRANT)
check("new sesid → normal call, B Granted (no B-bit)",
      g is not None and (u16(g, 13) or 0) & FI_BROADCAST == 0, f"indicator={u16(g, 13)}")
req("PTT_GROUP_REMOVE", {"group_id": GROUP}, S2)

# 7) 전환기 — 구 CSP 의 group_type:"broadcast" = broadcast=1
L1 = f"{GROUP2}::s1"
r = add(GROUP2, L1, group_type="broadcast", initiator_id=A_ID)
fport2 = r.get("payload", {}).get("floor_port")
socks2 = mk_socks()
join(GROUP2, L1, socks2)
socks2[B_ID][1].sendto(floor_msg(0, 0xBBBB0012, [(0, bytes([5, 0]))]), (MY_IP, fport2))
d = recv_floor(socks2[B_ID][1], FLOOR_DENY)
check("legacy group_type=broadcast → Deny #5", d is not None and u16(d, 2) == 5, f"cause={u16(d, 2)}")
req("PTT_GROUP_REMOVE", {"group_id": GROUP2}, L1)

ok = all(results)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
