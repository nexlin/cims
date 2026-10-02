#!/usr/bin/env python3
# CMP floor 대기열 스모크 — TS 24.380 §6.3.5.3.7 · §6.3.5.4.5 · §6.3.5.4.12 · §6.3.4.4.13
#   시험용 CMP 를 임시 설정으로 스스로 띄운다(라이브 CMP 에 돌리지 않는다).
#   ① 대기 중인 참가자의 Floor Release = 대기 요청 삭제 + 그 참가자에게 Floor Taken(점유 중) — 나중에 승인이 오지 않는다
#   ② 발언자가 아닌 참가자의 Floor Release(유휴) = 그 참가자에게 Floor Idle
#   ③ 인가되지 않은 참가자의 Queued Floor Requests 취소 = 결과 1(Not authorized), 대기열 그대로
#   ④ 인가된 참가자(chair)의 목록 없는 취소 = 전체 삭제 + 취소된 대기자에게 Cancel Notification
# 사용법: python3 tests/cmp_smoke_floor_queue.py [build/bin/cmp]
import json, os, shutil, socket, struct, subprocess, sys, tempfile, time

CMP_BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/bin/cmp")
if not os.access(CMP_BIN, os.X_OK):
    print(f"cmp binary not found: {CMP_BIN} (make cmp)")
    sys.exit(2)
IP = "127.0.0.1"
GROUP = "grp-smoke-floorq"
A_ID, B_ID, C_ID = "+82519990001", "+82519990002", "+82519990003"   # C = chair(관제 감독)

def free_base():
    for b in range(36000, 60000, 100):
        socks = []
        try:
            for p in range(b, b + 60):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((IP, p)); socks.append(s)
            return b
        except OSError:
            continue
        finally:
            for s in socks: s.close()
    raise SystemExit("no free port window")

base = free_base()
work = tempfile.mkdtemp(prefix="cmp_floorq_")
cfg = {"ServerIp": IP, "ServerPort": base + 50, "RtpIp": IP, "RtpStartPort": base, "RtpPoolSize": 1, "TranscodeSlots": 0,
       "TapPoolSize": 0, "AnnPlayers": 0, "PttRtpStartPort": base + 10, "PttMemberPoolSize": 4,
       "PttVideoStartPort": base + 20, "PttFloorStartPort": base + 30, "PttRtpPoolSize": 1, "PttMediaBufferMs": 0,
       "McVideoStartPort": base + 40, "McVideoMemberPoolSize": 0, "RtpWorkerCount": 1, "LogDir": work,
       "SystemId": "cmpq", "ServiceLogging": {"Dir": ""}, "Fm": {"Enable": False}}
with open(os.path.join(work, "cmp.json"), "w") as f:
    json.dump(cfg, f)
proc = subprocess.Popen([CMP_BIN, os.path.join(work, "cmp.json")], cwd=work,
                        stdout=open(os.path.join(work, "stdout.log"), "w"), stderr=subprocess.STDOUT)

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctrl.settimeout(1.0)
_trans = [8800100]
def req(cmd, payload, timeout=3.0):
    _trans[0] += 1
    msg = {"hdr": {"cmd": cmd, "node": "smoketest", "service": "mcptt", "sesid": f"{GROUP}::q::{_trans[0]}",
                   "trans_id": _trans[0], "type": "request", "ver": 2}, "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), (IP, base + 50))
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, _ = ctrl.recvfrom(16384)
        except socket.timeout:
            continue
        r = json.loads(data.decode())
        if r.get("hdr", {}).get("trans_id") == _trans[0]:
            return r
    return {}

def floor_msg(subtype, ssrc, fields=()):
    body = b""
    for fid, val in fields:
        body += bytes([fid, len(val)]) + val
        body += b"\0" * ((-(2 + len(val))) % 4)
    words = (12 + len(body)) // 4 - 1
    return bytes([0x80 | subtype, 204]) + struct.pack("!H", words) + struct.pack("!I", ssrc) + b"MCPT" + body

def fields_of(data):
    out, p = {}, 12
    while p + 2 <= len(data):
        fid, ln = data[p], data[p + 1]
        out[fid] = data[p + 2:p + 2 + ln]
        p += (2 + ln + 3) // 4 * 4
    return out

def recv_all(sock, timeout=0.8, total=3.0):
    """floor 메시지를 모은다 — timeout 동안 조용하면 끝, 전체 total 초를 넘기지 않는다."""
    got = []
    end = time.time() + total
    while time.time() < end:
        sock.settimeout(min(timeout, max(0.05, end - time.time())))
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            if got: break
            continue
        if len(data) >= 12 and data[1] == 204 and data[8:12] == b"MCPT":
            got.append((data[0] & 0x1F, fields_of(data)))
    return got

def types(msgs): return [t for t, _ in msgs]
def u16(b): return struct.unpack("!H", b[:2])[0] if b and len(b) >= 2 else -1

fails = []
def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + (f" — {detail}" if detail and not cond else ""))
    if not cond: fails.append(name)

try:
    hb = {}
    for _ in range(50):
        hb = req("HEARTBEAT", {}, 0.2)
        if hb: break
    check("cmp up", bool(hb))
    r = req("PTT_GROUP_ADD", {"group_id": GROUP, "group_type": "prearranged", "initiator_id": A_ID,
                              "members": f"{A_ID}:5:participant,{B_ID}:5:participant,{C_ID}:5:chair", "subid": "1"})
    fport = r.get("payload", {}).get("floor_port")
    check("group add", bool(fport), json.dumps(r)[:200])
    socks = {}
    for sid, role in ((A_ID, "participant"), (B_ID, "participant"), (C_ID, "chair")):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((IP, 0))
        f = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); f.bind((IP, 0))
        j = req("PTT_JOIN", {"group_id": GROUP, "session_id": sid, "user_ip": IP, "user_port": s.getsockname()[1],
                             "user_floor_port": f.getsockname()[1], "role": role, "queueing": 1})
        check(f"join {sid}", j.get("hdr", {}).get("status") == "OK", json.dumps(j)[:200])
        socks[sid] = (s, f)
    fa, fb, fc = socks[A_ID][1], socks[B_ID][1], socks[C_ID][1]
    to = (IP, fport)
    sa, sb, sc = 0xAAAA0001, 0xBBBB0002, 0xCCCC0003
    prio = [(0, bytes([5, 0]))]

    # A 발언, B 대기
    fa.sendto(floor_msg(0, sa, prio), to)
    check("A granted", 1 in types(recv_all(fa)))
    recv_all(fb); recv_all(fc)
    fb.sendto(floor_msg(0, sb, prio), to)
    check("B queued", 9 in types(recv_all(fb)))
    # ① B 가 PTT 를 뗀다 = Floor Release → 대기 삭제 + B 에게 Floor Taken
    fb.sendto(floor_msg(4, sb), to)
    check("B release while queued → Floor Taken", 2 in types(recv_all(fb)))
    fa.sendto(floor_msg(4, sa), to)
    after = types(recv_all(fb, 2.0, 4.0))
    check("A release → B gets Idle, no late Granted", 5 in after and 1 not in after, str(after))
    recv_all(fa); recv_all(fc)
    # ② 유휴 상태에서 발언자가 아닌 B 의 Floor Release → B 에게 Floor Idle
    fb.sendto(floor_msg(4, sb), to)
    check("non-talker release while idle → Floor Idle", 5 in types(recv_all(fb)))

    # ③ A 발언, B·C 대기 — B(인가 없음)의 목록 없는 취소 = 결과 1, 대기열 그대로
    fa.sendto(floor_msg(0, sa, prio), to); recv_all(fa)
    fb.sendto(floor_msg(0, sb, prio), to); recv_all(fb)
    fc.sendto(floor_msg(0, sc, prio), to); recv_all(fc)
    recv_all(fa)
    fb.sendto(floor_msg(0x0E, sb, [(21, struct.pack("!H", 0))]), to)
    res = [f for t, f in recv_all(fb) if t == 0x0E]
    check("unauthorized cancel → result 1", bool(res) and u16(res[0].get(21)) == 1 and u16(res[0].get(23)) == 1,
          str(res))
    # ④ C(chair)의 목록 없는 취소 = 전체 삭제, B 에게 Cancel Notification(2), C 에게 결과 0
    fc.sendto(floor_msg(0x0E, sc, [(21, struct.pack("!H", 0))]), to)
    resc = [f for t, f in recv_all(fc) if t == 0x0E]
    notb = [f for t, f in recv_all(fb) if t == 0x0E]
    check("chair cancel all → result 0", bool(resc) and u16(resc[0].get(23)) == 0, str(resc))
    check("queued B gets cancel notification", bool(notb) and u16(notb[0].get(21)) == 2, str(notb))
    fa.sendto(floor_msg(4, sa), to)
    after = types(recv_all(fb, 2.0, 4.0)) + types(recv_all(fc, 0.5))
    check("after cancel all → no Granted to B/C", 1 not in after, str(after))
    req("PTT_GROUP_REMOVE", {"group_id": GROUP})
finally:
    proc.terminate()
    try: proc.wait(5)
    except subprocess.TimeoutExpired: proc.kill()
    shutil.rmtree(work, ignore_errors=True)

print("RESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(0 if not fails else 1)
