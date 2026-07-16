#!/usr/bin/env python3
"""CMP Phase 1 긴급 floor 선점 라이브 점검 (cspsim 불필요).

CSP 가 하는 일을 그대로 CMP UDP 제어/floor 로 재현:
  PTT_GROUP_ADD(A,B) → JOIN A,B(floor 포트) → A floor REQUEST(GRANT)
  → PTT_FLOOR_TIER B emergency → B floor REQUEST → 선점(REVOKE A, GRANT B) 확인 → REMOVE.

floor REQUEST 는 RTCP-APP("MCPT") 패킷을 멤버 floorPort(=source port)에서 CMP shared floor port 로 송신.
CMP 가 source 포트로 멤버를 식별(onFloorPacket: floorPort 매칭 + IP 학습).
응답 floor 패킷(GRANT/TAKEN/REVOKE)을 멤버 소켓에서 직접 수신해 즉시 판정.
"""
import argparse
import json
import socket
import struct
import time

RTCP_PT_APP = 204
REQUEST, GRANT, REJECT, RELEASE, IDLE, TAKEN, REVOKE = 1, 2, 3, 4, 5, 6, 7
OPN = {1: "REQUEST", 2: "GRANT", 3: "REJECT", 4: "RELEASE", 5: "IDLE", 6: "TAKEN", 7: "REVOKE"}


def floor_pkt(opcode, ssrc=0x11223344):
    # 16 byte: ver(0x80) type(204) len(htons(3)) ssrc name="MCPT" opcode id_len=0 reserved=0
    return struct.pack(">BBHI4sBBH", 0x80, RTCP_PT_APP, 3, ssrc, b"MCPT", opcode, 0, 0)


def parse_floor(buf):
    if len(buf) < 16:
        return None
    op = buf[12]
    return OPN.get(op, "?")


def ctl(sock, ip, port, payload, tid):
    # envelope v2 (docs/api/cmp_media_api.md) — payload 의 cmd 는 hdr 로 승격
    p = dict(payload)
    pkt = {"hdr": {"ver": 2, "trans_id": tid, "node": "script",
                   "cmd": p.pop("cmd", ""), "type": "request", "service": "mcptt"}}
    if p:
        pkt["payload"] = p
    sock.sendto(json.dumps(pkt).encode(), (ip, port))
    try:
        data, _ = sock.recvfrom(8192)
        return json.loads(data.decode())
    except socket.timeout:
        return None


def ctl_result(r):
    """응답 표시용 — hdr.status (+ payload)"""
    if not r:
        return "(no resp)"
    hdr = r.get("hdr") or {}
    out = hdr.get("status", "?")
    if hdr.get("code"):
        out += f" {hdr['code']}"
    if r.get("payload"):
        out += f" {r['payload']}"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmp", required=True)
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--group", default="ztest_emerg")
    ap.add_argument("--record-dir", default="/mnt/cims/service_log/ptt/ztest_emerg")
    ap.add_argument("--fport-a", type=int, default=51140)
    ap.add_argument("--fport-b", type=int, default=51142)
    a = ap.parse_args()

    ctlsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctlsock.settimeout(3.0)
    tid = int(time.time()) % 100000

    # 멤버 floor 소켓 (source port = floorPort)
    sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sa.bind(("0.0.0.0", a.fport_a)); sa.settimeout(2.0)
    sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sb.bind(("0.0.0.0", a.fport_b)); sb.settimeout(2.0)
    myip = socket.gethostbyname(socket.gethostname())

    def step(label, payload):
        nonlocal tid
        tid += 1
        r = ctl(ctlsock, a.cmp, a.port, payload, tid)
        print(f"  {label}: {ctl_result(r)}")
        return r

    print(f"[1] PTT_GROUP_ADD {a.group} (A,B)")
    r = step("ADD", {"cmd": "PTT_GROUP_ADD", "group_id": a.group,
                     "members": "A:5:participant,B:5:participant", "count": 2,
                     "record_dir": a.record_dir, "log_dir": a.record_dir})
    floor_port = (r.get("payload") or {}).get("floor_port") if r else None
    if not floor_port:
        print("  ✗ floor_port 미확보 — 중단"); return
    print(f"    shared floor_port = {floor_port}")

    print("[2] JOIN A,B")
    step("JOIN A", {"cmd": "PTT_JOIN", "group_id": a.group, "session_id": "A",
                    "user_ip": myip, "user_port": a.fport_a - 1, "user_floor_port": a.fport_a, "role": "participant"})
    step("JOIN B", {"cmd": "PTT_JOIN", "group_id": a.group, "session_id": "B",
                    "user_ip": myip, "user_port": a.fport_b - 1, "user_floor_port": a.fport_b, "role": "participant"})

    def drain(s, who):
        got = []
        try:
            while True:
                d, _ = s.recvfrom(2048)
                op = parse_floor(d)
                if op:
                    got.append(op)
        except socket.timeout:
            pass
        if got:
            print(f"    [{who}] 수신 floor: {got}")
        return got

    print("[3] A floor REQUEST (정상 — GRANT 기대)")
    sa.sendto(floor_pkt(REQUEST), (a.cmp, floor_port))
    time.sleep(0.4); drain(sa, "A"); drain(sb, "B")

    print("[4] PTT_FLOOR_TIER B = emergency")
    step("TIER", {"cmd": "PTT_FLOOR_TIER", "group_id": a.group, "session_id": "B", "tier": "emergency"})

    print("[5] B floor REQUEST (긴급 — A 선점 기대: A REVOKE, B GRANT)")
    sb.sendto(floor_pkt(REQUEST), (a.cmp, floor_port))
    time.sleep(0.5)
    ga = drain(sa, "A"); gb = drain(sb, "B")
    verdict = ("REVOKE" in ga) and ("GRANT" in gb)
    print(f"\n  ▶ 판정: A에 REVOKE={'O' if 'REVOKE' in ga else 'X'} / B에 GRANT={'O' if 'GRANT' in gb else 'X'}  → "
          + ("✅ 긴급 선점 확인" if verdict else "⚠ 패킷 수신으로는 미확정 — floor.jsonl 확인"))

    print("[6] PTT_GROUP_REMOVE (정리)")
    step("REMOVE", {"cmd": "PTT_GROUP_REMOVE", "group_id": a.group})
    print(f"\nfloor.jsonl 확인: {a.record_dir}/<YYYY/MM/DD/HH>/floor.jsonl  (reason:emergency_preempt 기대)")


if __name__ == "__main__":
    main()
