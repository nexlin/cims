#!/usr/bin/env python3
"""CMP floor control TS 24.380 정합 라이브 점검 (cspsim 불필요, CMP UDP 직접).

CSP 가 하는 일을 CMP UDP 제어로 재현하고, **TS 24.380 subtype+TLV** floor 패킷을 멤버 소켓에서
직접 주고받아 판정한다(단말 floor/FloorCodec.kt 와 동일 규약). 검증 시나리오:
  1) A REQUEST → A GRANTED(Duration/Granted Party/Indicator) + 전체 TAKEN
  2) B REQUEST(동prio, 비선점) → B QUEUE_POS_INFO (큐잉; Deny 아님)
  3) A RELEASE → 대기자 B 자동 GRANTED + TAKEN
  4) B RELEASE → IDLE
  5) (신규 그룹) PTT_FLOOR_TIER B=emergency → A GRANT 후 B REQUEST → A REVOKE(cause) + B GRANTED

subtype: Request=0 Granted=1 Taken=2 Deny=3 Release=4 Idle=5 Revoke=6 QueuePosReq=8 QueuePosInfo=9 Ack=10
"""
import argparse, json, socket, struct, time

PT_APP = 204
REQUEST, GRANTED, TAKEN, DENY, RELEASE, IDLE, REVOKE, QPREQ, QPINFO, ACK = 0, 1, 2, 3, 4, 5, 6, 8, 9, 10
NAME = {REQUEST:"REQUEST",GRANTED:"GRANTED",TAKEN:"TAKEN",DENY:"DENY",RELEASE:"RELEASE",
        IDLE:"IDLE",REVOKE:"REVOKE",QPREQ:"QPREQ",QPINFO:"QPINFO",ACK:"ACK"}
# Field IDs (TS 24.380 §8.2.3)
F_PRIO, F_DUR, F_CAUSE, F_QINFO, F_GPARTY, F_USERID, F_QSIZE, F_INDIC = 0, 1, 2, 3, 4, 6, 7, 13
F_MSGSEQ, F_PERM, F_SSRC = 8, 5, 14

def _pad4(n): return (4 - (n % 4)) % 4

def encode(subtype, ssrc, fields):
    # §8.1.3 — 모든 필드는 패딩 포함 4옥텟 배수
    body = b""
    for fid, val in fields:
        body += bytes([fid & 0xFF, len(val) & 0xFF]) + val
        body += b"\x00" * _pad4(2 + len(val))
    body += b"\x00" * _pad4(len(body))
    total = 12 + len(body)
    words = total // 4 - 1
    hdr = struct.pack(">BBHI4s", 0x80 | (subtype & 0x1F), PT_APP, words, ssrc, b"MCPT")
    return hdr + body

def decode(buf):
    if len(buf) < 12 or (buf[0] & 0xC0) != 0x80 or buf[1] != PT_APP or buf[8:12] != b"MCPT":
        return None
    subtype = buf[0] & 0x1F
    ssrc = struct.unpack(">I", buf[4:8])[0]
    fields, p = {}, 12
    while p + 2 <= len(buf):
        fid = buf[p]
        hdr = 3 if fid >= 192 else 2          # ID>=192 는 Length 2옥텟 (§8.1.3)
        if p + hdr > len(buf): break
        fl = struct.unpack(">H", buf[p+1:p+3])[0] if hdr == 3 else buf[p+1]
        if fid == 0 and fl == 0: break
        if p + hdr + fl > len(buf): break
        fields[fid] = buf[p+hdr:p+hdr+fl]
        p += hdr + fl + _pad4(hdr + fl)       # 모든 필드 4옥텟 정렬
    return {"subtype": subtype & 0x0F, "raw_subtype": subtype, "ssrc": ssrc, "fields": fields}

def req(ssrc, userid, prio=5):
    return encode(REQUEST, ssrc, [(F_PRIO, bytes([prio, 0])), (F_USERID, userid.encode())])
def rel(ssrc, userid):
    return encode(RELEASE, ssrc, [(F_USERID, userid.encode())])

def u16(fields, fid):
    v = fields.get(fid)
    return (v[0] << 8) | v[1] if v and len(v) >= 2 else None

PASS, FAIL = [], []
def check(cond, msg):
    (PASS if cond else FAIL).append(msg)
    print(f"    {'✅' if cond else '❌'} {msg}")

def ctl(sock, ip, port, payload, tid):
    # envelope v2 (docs/api/cmp_media_api.md) — payload 의 cmd 는 hdr 로 승격
    p = dict(payload)
    pkt = {"hdr": {"ver": 2, "trans_id": tid, "node": "script",
                   "cmd": p.pop("cmd", ""), "type": "request", "service": "mcptt"}}
    if p:
        pkt["payload"] = p
    sock.sendto(json.dumps(pkt).encode(), (ip, port))
    # CMP 는 같은 소켓으로 이벤트(FLOOR_TALKERS 등)도 보낸다 — trans_id 로 응답만 골라낸다.
    deadline = time.time() + 3.0
    while time.time() < deadline:
        try:
            r = json.loads(sock.recvfrom(8192)[0].decode())
        except socket.timeout:
            return None
        h = r.get("hdr") or {}
        if h.get("type") == "response" and h.get("trans_id") in (tid, str(tid)):
            return r
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmp", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--rec", default="/tmp/floor_codec_live")
    a = ap.parse_args()

    ctlsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctlsock.settimeout(3.0)
    # CMP 로 나가는 실제 소스 IP — 선언 주소(user_ip)와 다르면 CMP 가 미협상 소스로 드롭한다.
    _p = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); _p.connect((a.cmp, a.port))
    myip = _p.getsockname()[0]; _p.close()
    tid = int(time.time()) % 100000
    def step(label, payload):
        nonlocal tid; tid += 1
        r = ctl(ctlsock, a.cmp, a.port, payload, tid)
        st = (r.get("hdr") or {}).get("status") if r else None
        print(f"  · {label}: {st or '(no resp)'}{' ' + str(r.get('payload')) if r and r.get('payload') else ''}")
        return r

    def run_group(group, fpa, fpb):
        sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sa.bind(("0.0.0.0", fpa)); sa.settimeout(1.5)
        sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sb.bind(("0.0.0.0", fpb)); sb.settimeout(1.5)
        # 이 스크립트는 미디어를 보내지 않는 **wire/코덱** 시험이라 T1(무RTP 발언 종료)·T3(회수
        #   유예)가 단계 사이에 끼어들면 안 된다 — 길게 잡는다(타이머 동작은 floor 정책 프로브가 검증).
        r = step(f"PTT_GROUP_ADD {group}", {"cmd":"PTT_GROUP_ADD","group_id":group,
                 "members":"A:5:participant,B:5:participant","count":2,
                 "floor_timers":{"t1_end_rtp":120,"t2_stop_talk":0,"t3_grace":10},
                 "record_dir":f"{a.rec}/{group}","log_dir":f"{a.rec}/{group}"})
        fport = (r.get("payload") or {}).get("floor_port") if r else None
        if not fport:
            check(False, f"{group}: floor_port 확보"); return None, None, None, None
        step("JOIN A", {"cmd":"PTT_JOIN","group_id":group,"session_id":"A","user_ip":myip,
                        "user_port":fpa-1,"user_floor_port":fpa,"role":"participant"})
        step("JOIN B", {"cmd":"PTT_JOIN","group_id":group,"session_id":"B","user_ip":myip,
                        "user_port":fpb-1,"user_floor_port":fpb,"role":"participant"})
        return sa, sb, fport, group

    def drain(s):
        out = []
        try:
            while True:
                out.append(decode(s.recvfrom(2048)[0]))
        except socket.timeout:
            pass
        return [m for m in out if m]
    def subtypes(msgs): return [NAME.get(m["subtype"], m["subtype"]) for m in msgs]
    def find(msgs, st): return next((m for m in msgs if m["subtype"] == st), None)

    print("\n=== 시나리오 A: grant → queue → release-advance → idle ===")
    sa, sb, fport, g = run_group("zcodec_q", 51140, 51142)
    if sa:
        print("[1] A REQUEST → GRANTED + TAKEN")
        sa.sendto(req(0xA1, "tel:+8210000001"), (a.cmp, fport)); time.sleep(0.4)
        ma, mb = drain(sa), drain(sb)
        ga = find(ma, GRANTED)
        check(ga is not None, "A 가 GRANTED(1) 수신")
        if ga:
            check(u16(ga["fields"], F_DUR) is not None, f"GRANTED Duration TLV (={u16(ga['fields'],F_DUR)}s)")
            # §8.2.5 — Granted 는 화자 SSRC 를 SSRC 필드(14)로 싣는다(Granted Party 는 Taken 전용)
            check(F_SSRC in ga["fields"], "GRANTED SSRC TLV (§8.2.3.16)")
            check(u16(ga["fields"], F_INDIC) is not None, f"GRANTED Floor Indicator TLV (=0x{(u16(ga['fields'],F_INDIC) or 0):04x})")
        tk = find(mb, TAKEN)
        check(tk is not None, "TAKEN(2) 브로드캐스트 수신 (화자 외)")
        if tk:
            # §8.2.9 — Granted Party + Permission to Request the Floor + Message Seq Number
            check(F_GPARTY in tk["fields"], f"TAKEN Granted Party TLV (={tk['fields'].get(F_GPARTY,b'').decode(errors='replace')})")
            check(u16(tk["fields"], F_PERM) == 1, "TAKEN Permission to Request the Floor=1")
            check(u16(tk["fields"], F_MSGSEQ) is not None, "TAKEN Message Sequence Number TLV")

        print("[2] B REQUEST (동prio 비선점) → QUEUE_POS_INFO")
        sb.sendto(req(0xB1, "tel:+8210000002"), (a.cmp, fport)); time.sleep(0.4)
        mb = drain(sb)
        qp = find(mb, QPINFO)
        check(qp is not None, "B 가 QUEUE_POS_INFO(9) 수신 (Deny 아님 — 큐잉)")
        check(find(mb, DENY) is None, "B 가 DENY 받지 않음")
        if qp:
            check(F_QINFO in qp["fields"], f"QUEUE_POS_INFO Queue Info TLV (pos={qp['fields'].get(F_QINFO,b'\\x00')[0]})")

        print("[3] A RELEASE → 대기자 B 자동 GRANTED")
        sa.sendto(rel(0xA1, "tel:+8210000001"), (a.cmp, fport)); time.sleep(0.5)
        ma, mb = drain(sa), drain(sb)
        check(find(mb, GRANTED) is not None, "B 가 자동 GRANTED(1) 수신 (큐 승계)")

        print("[4] B RELEASE → IDLE")
        sb.sendto(rel(0xB1, "tel:+8210000002"), (a.cmp, fport)); time.sleep(0.5)
        ma, mb = drain(sa), drain(sb)
        check(find(ma, IDLE) or find(mb, IDLE), "IDLE(5) 브로드캐스트 수신")
        step("REMOVE", {"cmd":"PTT_GROUP_REMOVE","group_id":g})
        sa.close(); sb.close()

    print("\n=== 시나리오 B: emergency 선점 (REVOKE cause + GRANTED) ===")
    sa, sb, fport, g = run_group("zcodec_emerg", 51150, 51152)
    if sa:
        print("[1] A REQUEST → GRANTED")
        sa.sendto(req(0xA2, "tel:+8210000001"), (a.cmp, fport)); time.sleep(0.4)
        drain(sa); drain(sb)
        print("[2] PTT_FLOOR_TIER B = emergency")
        step("TIER", {"cmd":"PTT_FLOOR_TIER","group_id":g,"session_id":"B","tier":"emergency"})
        # 선점은 'G: pending Floor Revoke'(§6.3.4.5)를 거친다 — 기존 화자에 REVOKE 를 보내고
        #   T3 유예 동안 Floor Release 를 기다리며, 요청자는 큐 선두에서 대기한다.
        print("[3] B REQUEST (긴급) → A REVOKE + B 큐 선두 대기")
        sb.sendto(req(0xB2, "tel:+8210000002"), (a.cmp, fport)); time.sleep(0.5)
        # B 먼저 확인 — A 는 유예 동안 T8 로 REVOKE 를 반복 수신하므로 나중에 비운다.
        mb = drain(sb)
        check(find(mb, QPINFO) is not None, "B 가 QUEUE_POS_INFO(9) 수신 (회수 유예 대기)")
        check(find(mb, GRANTED) is None, "유예 중에는 GRANTED 를 보내지 않는다")

        print("[4] A RELEASE (회수 응답) → B GRANTED")
        sa.sendto(rel(0xA2, "tel:+8210000001"), (a.cmp, fport)); time.sleep(0.5)
        ma, mb = drain(sa), drain(sb)
        rv = find(ma, REVOKE)
        check(rv is not None, "A 가 REVOKE(6) 수신")
        if rv:
            check(u16(rv["fields"], F_CAUSE) == 4, f"REVOKE cause #4 Media Burst pre-empted (={u16(rv['fields'],F_CAUSE)})")
        gb = find(mb, GRANTED)
        check(gb is not None, "B(긴급) 가 GRANTED(1) 수신")
        if gb:
            ind = u16(gb["fields"], F_INDIC) or 0
            check((ind & 0x1000) != 0, f"B GRANTED Floor Indicator=emergency 비트(0x1000) (=0x{ind:04x})")
        step("REMOVE", {"cmd":"PTT_GROUP_REMOVE","group_id":g})
        sa.close(); sb.close()

    print(f"\n{'='*48}\n결과: {len(PASS)} PASS / {len(FAIL)} FAIL")
    for m in FAIL: print(f"  ❌ {m}")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
