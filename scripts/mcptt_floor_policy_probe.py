#!/usr/bin/env python3
"""CMP floor 정책 라이브 프로브 — floor_control / floor_policy(dual·multi) / private call /
ambient(recv_only·floor_suppress) / floor_crypto(SRTCP) / STATS floor_holders.

CSP 없이 CMP 제어평면(UDP JSON envelope v2)과 floor RTCP(RTCP APP "MCPT")를 직접 구동한다.
정본 규격: docs/api/cmp_media_api.md §7 (PTT_*), docs/design/features/mcptt_csp_cmp_roadmap_contract.md §B.

사용:
  python3 scripts/mcptt_floor_policy_probe.py --cmp 192.168.0.x [--port 9000] [--base-port 51200]

`--base-port` 부터 약 110 포트를 멤버 leg 소켓으로 bind 한다 — CMP 자신의 RTP/floor 풀
(cmp.json `RtpStartPort`/`PttRtpStartPort`/`PttFloorStartPort` 이후 대역)과 겹치지 않는 값을 준다.

주의: CMP 는 요청 소스에서 이벤트 전송 대상(CSP endpoint)을 학습한다. 프로브 실행 중에는
FLOOR_TALKERS 등 이벤트가 이 스크립트로 오고, 실제 CSP 는 다음 HEARTBEAT(3s)에 다시 학습된다.
"""
import argparse
import base64
import hashlib
import hmac
import json
import socket
import struct
import sys
import time

RTCP_PT_APP = 204
# TS 24.380 Table 8.2.2-1 subtype
REQUEST, GRANT, TAKEN, DENY, RELEASE, IDLE, REVOKE = 0, 1, 2, 3, 4, 5, 6
QUEUE_POS_REQ, QUEUE_POS_INFO, ACK, RELEASE_MULTI = 8, 9, 10, 0x0F
OPN = {REQUEST: "REQUEST", GRANT: "GRANT", TAKEN: "TAKEN", DENY: "DENY", RELEASE: "RELEASE",
       IDLE: "IDLE", REVOKE: "REVOKE", QUEUE_POS_REQ: "QPOS_REQ", QUEUE_POS_INFO: "QPOS_INFO",
       ACK: "ACK", RELEASE_MULTI: "RELEASE_MULTI"}
ACK_REQ_BIT = 0x10          # subtype 첫 비트 = Acknowledgment is required (§8.2.2)
FF_PRIORITY, FF_DURATION, FF_CAUSE, FF_QUEUE_INFO = 0, 1, 2, 3
FF_GRANTED_PARTY, FF_PERMISSION = 4, 5
FF_USER_ID, FF_MSG_SEQ, FF_SOURCE, FF_MSG_TYPE, FF_INDICATOR = 6, 8, 10, 12, 13
FF_SSRC, FF_GRANTED_USERS, FF_SSRC_LIST = 14, 15, 16
FI_EMERGENCY, FI_DUAL, FI_MULTI = 0x1000, 0x0200, 0x0080

PASS, FAIL = [], []


def check(cond, msg):
    (PASS if cond else FAIL).append(msg)
    print(f"    {'✓' if cond else '✗'} {msg}")


# ── floor 코덱 (cmp/PFloorCodec.cpp 와 동일 규약) ────────────────────────────
def _pad4(n):
    return (4 - (n % 4)) % 4


def floor_build(subtype, ssrc, fields):
    # §8.1.3 — 모든 필드는 패딩 포함 4옥텟 배수다(문자열 필드만 정렬하는 게 아니다).
    body = b""
    for fid, val in fields:
        body += bytes([fid, len(val)]) + val
        body += b"\x00" * _pad4(2 + len(val))
    body += b"\x00" * _pad4(len(body))
    total = 12 + len(body)
    return struct.pack(">BBHI4s", 0x80 | (subtype & 0x1F), RTCP_PT_APP, total // 4 - 1,
                       ssrc, b"MCPT") + body


def floor_parse(buf):
    if len(buf) < 12 or buf[8:12] != b"MCPT":
        return None
    sub = buf[0] & 0x1F
    out = {"subtype": sub, "op_base": sub & 0x0F, "ack_req": bool(sub & ACK_REQ_BIT),
           "ssrc": struct.unpack(">I", buf[4:8])[0], "fields": {}}
    p = 12
    while p + 2 <= len(buf):
        fid = buf[p]
        hdr = 3 if fid >= 192 else 2
        if p + hdr > len(buf):
            break
        ln = struct.unpack(">H", buf[p + 1:p + 3])[0] if hdr == 3 else buf[p + 1]
        if fid == 0 and ln == 0:
            break
        if p + hdr + ln > len(buf):
            break
        out["fields"][fid] = buf[p + hdr:p + hdr + ln]
        p += hdr + ln
        p += _pad4(hdr + ln)          # 필드 단위 4옥텟 정렬
    out["op"] = OPN.get(out["op_base"], f"?{sub}")
    return out


def req_pkt(user, ssrc, prio=5, emergency=False):
    f = [(FF_PRIORITY, bytes([prio, 0])), (FF_USER_ID, user.encode())]
    if emergency:
        f.append((FF_INDICATOR, struct.pack(">H", FI_EMERGENCY)))
    return floor_build(REQUEST, ssrc, f)


def rel_pkt(user, ssrc, multi=False, ack_req=False):
    """Floor Release. ack_req=True 면 subtype 0x14(확인 요구) — 단말이 실제로 쓰는 변종."""
    sub = RELEASE_MULTI if multi else (RELEASE | (ACK_REQ_BIT if ack_req else 0))
    return floor_build(sub, ssrc, [(FF_USER_ID, user.encode())])


def user_list(val):
    """List of Granted Users(§8.2.3.17) 디코드 → [user, ...]"""
    out, n, p = [], val[0] if val else 0, 1
    for _ in range(n):
        if p >= len(val):
            break
        ln = val[p]
        out.append(val[p + 1:p + 1 + ln].decode(errors="ignore"))
        p += 1 + ln
    return out


def ssrc_list(val):
    """List of SSRCs(§8.2.3.18) 디코드 → [ssrc, ...]"""
    n = val[0] if val else 0
    return [struct.unpack(">I", val[2 + 4 * i:6 + 4 * i])[0] for i in range(n) if 6 + 4 * i <= len(val)]


# ── SRTCP (RFC 3711, AES_CM_128_HMAC_SHA1_80) — cmp/PFloorCrypto.cpp 대응 ────
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    HAVE_CRYPTO = True
except Exception:
    HAVE_CRYPTO = False


class Srtcp:
    def __init__(self, key, salt, tag_len=10):
        self.key, self.salt, self.tag_len = key, salt, tag_len
        self.sess_key = self._kdf(0x03, 16)
        self.sess_auth = self._kdf(0x04, 20)
        self.sess_salt = self._kdf(0x05, 14)
        self.index = 0

    def _ctr(self, key, iv, n):
        c = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
        return c.update(b"\x00" * n) + c.finalize()

    def _kdf(self, label, n):
        x = bytearray(self.salt + b"\x00\x00")
        x[7] ^= label
        return self._ctr(self.key, bytes(x), n)

    def _iv(self, ssrc, index):
        iv = bytearray(self.sess_salt + b"\x00\x00")
        for k, b in enumerate(struct.pack(">I", ssrc)):
            iv[4 + k] ^= b
        for k, b in enumerate(struct.pack(">I", index)):
            iv[10 + k] ^= b
        return bytes(iv)

    def _crypt(self, ssrc, index, data):
        ks = self._ctr(self.sess_key, self._iv(ssrc, index), len(data))
        return bytes(a ^ b for a, b in zip(data, ks))

    def protect(self, pkt):
        ssrc = struct.unpack(">I", pkt[4:8])[0]
        idx = self.index
        self.index += 1
        body = self._crypt(ssrc, idx, pkt[8:])
        out = pkt[:8] + body + struct.pack(">I", idx | 0x80000000)
        tag = hmac.new(self.sess_auth, out, hashlib.sha1).digest()[:self.tag_len]
        return out + tag

    def unprotect(self, pkt):
        if len(pkt) < 8 + 4 + self.tag_len:
            return None
        tag_off = len(pkt) - self.tag_len
        idx_off = tag_off - 4
        want = hmac.new(self.sess_auth, pkt[:idx_off + 4], hashlib.sha1).digest()[:self.tag_len]
        if not hmac.compare_digest(want, pkt[tag_off:]):
            return None
        e_index = struct.unpack(">I", pkt[idx_off:idx_off + 4])[0]
        ssrc = struct.unpack(">I", pkt[4:8])[0]
        body = pkt[8:idx_off]
        if e_index & 0x80000000:
            body = self._crypt(ssrc, e_index & 0x7FFFFFFF, body)
        return pkt[:8] + body


# ── 제어평면 ────────────────────────────────────────────────────────────────
class Ctl:
    def __init__(self, ip, port):
        self.ip, self.port = ip, port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(3.0)
        self.tid = int(time.time() * 1000) % 100000

    def send(self, cmd, **payload):
        self.tid += 1
        env = {"hdr": {"ver": 2, "trans_id": self.tid, "node": "probe", "cmd": cmd,
                       "type": "request", "service": "mcptt"}}
        if payload:
            env["payload"] = payload
        self.s.sendto(json.dumps(env).encode(), (self.ip, self.port))
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                data, _ = self.s.recvfrom(8192)
            except socket.timeout:
                return None
            r = json.loads(data.decode())
            hdr = r.get("hdr") or {}
            if hdr.get("type") == "event":   # 이벤트가 섞여 오면 ack 후 계속 대기
                self.events.append(r)
                ack = {"hdr": {"ver": 2, "trans_id": hdr.get("trans_id"), "node": "probe",
                               "cmd": hdr.get("cmd"), "type": "response", "status": "OK"}}
                self.s.sendto(json.dumps(ack).encode(), (self.ip, self.port))
                continue
            return r
        return None

    events = []

    def drain_events(self, sec=0.6):
        end = time.time() + sec
        while time.time() < end:
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                data, _ = self.s.recvfrom(8192)
            except socket.timeout:
                break
            r = json.loads(data.decode())
            hdr = r.get("hdr") or {}
            if hdr.get("type") == "event":
                self.events.append(r)
                ack = {"hdr": {"ver": 2, "trans_id": hdr.get("trans_id"), "node": "probe",
                               "cmd": hdr.get("cmd"), "type": "response", "status": "OK"}}
                self.s.sendto(json.dumps(ack).encode(), (self.ip, self.port))
        self.s.settimeout(3.0)
        return self.events


def status(r):
    return ((r or {}).get("hdr") or {}).get("status", "(no resp)")


def code(r):
    return ((r or {}).get("hdr") or {}).get("code", "")


def payload(r):
    return (r or {}).get("payload") or {}


class Member:
    """멤버 leg — floor 소켓(=user_floor_port) + audio 소켓(=user_port)."""

    def __init__(self, sid, base, ssrc, crypto=None):
        self.sid, self.ssrc, self.crypto = sid, ssrc, crypto
        self.fsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.fsock.bind(("0.0.0.0", base))
        self.fsock.settimeout(0.4)
        self.asock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.asock.bind(("0.0.0.0", base + 2))
        self.asock.settimeout(0.4)
        self.fport, self.aport = base, base + 2

    def send_floor(self, cmp_ip, floor_port, pkt):
        if self.crypto:
            pkt = self.crypto.protect(pkt)
        self.fsock.sendto(pkt, (cmp_ip, floor_port))

    def send_rtp(self, cmp_ip, port, seq=1, pt=8):
        hdr = struct.pack(">BBHII", 0x80, pt, seq, 1000 * seq, self.ssrc)
        self.asock.sendto(hdr + b"\xd5" * 160, (cmp_ip, port))

    def drain_floor(self):
        got = []
        while True:
            try:
                d, _ = self.fsock.recvfrom(2048)
            except socket.timeout:
                break
            if self.crypto:
                d = self.crypto.unprotect(d)
                if d is None:
                    got.append({"op": "AUTH_FAIL", "op_base": -1, "subtype": -1, "fields": {}})
                    continue
            m = floor_parse(d)
            if m:
                got.append(m)
        return got

    def drain_rtp(self):
        n = 0
        while True:
            try:
                self.asock.recvfrom(2048)
                n += 1
            except socket.timeout:
                break
        return n

    def close(self):
        self.fsock.close()
        self.asock.close()


def ops(msgs):
    return [m["op"] for m in msgs]


def granted_parties(msgs, op=TAKEN):
    return [m["fields"].get(FF_GRANTED_PARTY, b"").decode(errors="ignore").rstrip("\x00")
            for m in msgs if m["op_base"] == op]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmp", required=True)
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--base-port", type=int, default=51200)
    ap.add_argument("--prefix", default="zprobe")
    a = ap.parse_args()

    ctl = Ctl(a.cmp, a.port)
    # CMP 로 나가는 실제 소스 IP — 선언 주소(user_ip)와 달라지면 CMP 가 소스 미협상으로
    #   드롭한다(호스트명 조회는 127.0.1.1 등을 주므로 라우팅 기준으로 구한다).
    _p = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    _p.connect((a.cmp, a.port))
    myip = _p.getsockname()[0]
    _p.close()
    print(f"probe src ip = {myip}")
    bp = a.base_port

    def add_group(gid, **kw):
        return ctl.send("PTT_GROUP_ADD", group_id=gid, sesid=f"probe_{gid}", **kw)

    def join(gid, m, **kw):
        return ctl.send("PTT_JOIN", group_id=gid, session_id=m.sid, user_ip=myip,
                        user_port=m.aport, user_floor_port=m.fport, role="participant", **kw)

    def remove(gid):
        ctl.send("PTT_GROUP_REMOVE", group_id=gid)

    # ── 1. 단일 화자 회귀 ────────────────────────────────────────────────
    print("\n[1] single (회귀) — GRANT/TAKEN, 2번째 요청은 큐, RELEASE 시 IDLE")
    g1 = f"{a.prefix}_single"
    A = Member("A", bp, 0x1001)
    B = Member("B", bp + 4, 0x1002)
    r = add_group(g1, members="A:5:participant,B:5:participant")
    fp = payload(r).get("floor_port")
    check(status(r) == "OK" and fp, f"ADD ok floor_port={fp}")
    join(g1, A)
    join(g1, B)
    A.drain_floor(); B.drain_floor()

    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.3)
    ga, gb = A.drain_floor(), B.drain_floor()
    check("GRANT" in ops(ga), f"A GRANT (A={ops(ga)})")
    check("TAKEN" in ops(gb), f"B TAKEN (B={ops(gb)})")
    # 규격 필드 (TS 24.380 §8.2.5/§8.2.9): 헤더 SSRC 는 서버 SSRC, 화자 SSRC 는 SSRC 필드(14).
    grant = [m for m in ga if m["op_base"] == GRANT][0]
    taken = [m for m in gb if m["op_base"] == TAKEN][0]
    srv = grant["ssrc"]
    check(srv not in (A.ssrc, B.ssrc, 0), f"GRANT 헤더 SSRC = 서버 SSRC ({srv:#x})")
    check(taken["ssrc"] == srv, f"TAKEN 헤더 SSRC 도 서버 SSRC ({taken['ssrc']:#x})")
    check(FF_SSRC in grant["fields"] and
          struct.unpack(">I", grant["fields"][FF_SSRC][:4])[0] == A.ssrc,
          "GRANT SSRC 필드 = 화자 SSRC")
    check(FF_DURATION in grant["fields"], "GRANT Duration 필드")
    check(taken["fields"].get(FF_PERMISSION) == b"\x00\x01", "TAKEN Permission to Request the Floor=1")
    check(FF_MSG_SEQ in taken["fields"], "TAKEN Message Sequence Number 필드")
    check(FF_SSRC in taken["fields"], "TAKEN SSRC 필드(단일 화자)")
    check("TAKEN" not in ops(ga), f"화자 본인에게는 TAKEN 미송신 (A={ops(ga)})")

    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))
    time.sleep(0.3)
    gb = B.drain_floor()
    check("QPOS_INFO" in ops(gb), f"B 큐 대기 (B={ops(gb)})")

    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g1]
    check(grp and grp[0].get("floor_holders") == ["A"], f"STATS floor_holders={grp and grp[0].get('floor_holders')}")

    A.send_floor(a.cmp, fp, rel_pkt("A", A.ssrc))
    time.sleep(0.3)
    gb = B.drain_floor()
    check("GRANT" in ops(gb), f"큐 승급으로 B GRANT (B={ops(gb)})")
    # 마지막 해제는 Ack 요구 변종(subtype 0x14)으로 — 서버가 이를 무시하면 발언권이 고착된다.
    B.send_floor(a.cmp, fp, rel_pkt("B", B.ssrc, ack_req=True))
    time.sleep(0.3)
    gb2, ga2 = B.drain_floor(), A.drain_floor()
    check("IDLE" in ops(ga2), f"ack 요구 RELEASE(0x14) 처리 후 IDLE (A={ops(ga2)})")
    acks = [m for m in gb2 if m["op_base"] == ACK]
    check(bool(acks), f"Floor Ack 회신 (B={ops(gb2)})")
    if acks:
        check(acks[0]["fields"].get(FF_SOURCE) == b"\x00\x02", "Floor Ack Source=controlling(2)")
        check(acks[0]["fields"].get(FF_MSG_TYPE, b"\x00")[0] == (RELEASE | ACK_REQ_BIT),
              "Floor Ack Message Type = 확인 대상 subtype")
    idle = [m for m in ga2 if m["op_base"] == IDLE][0]
    check(FF_MSG_SEQ in idle["fields"] and FF_INDICATOR in idle["fields"],
          "IDLE 에 MSN·Floor Indicator 필드 (§8.2.8)")
    remove(g1)
    A.close(); B.close()

    # ── 2. dual floor — override 전용 2번째 자리 ─────────────────────────
    print("\n[2] dual — 동급 요청은 큐, 긴급 요청은 REVOKE 없이 동시 GRANT")
    g2 = f"{a.prefix}_dual"
    A = Member("A", bp + 10, 0x2001)
    B = Member("B", bp + 14, 0x2002)
    r = add_group(g2, members="A:5:participant,B:5:participant", floor_policy="dual")
    fp = payload(r).get("floor_port")
    check(status(r) == "OK", f"ADD dual ok (floor_port={fp})")
    join(g2, A); join(g2, B)
    A.drain_floor(); B.drain_floor()

    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.25); A.drain_floor(); B.drain_floor()
    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))
    time.sleep(0.25)
    gb = B.drain_floor()
    check("QPOS_INFO" in ops(gb) and "GRANT" not in ops(gb), f"동급 2번째는 동시 GRANT 아님 (B={ops(gb)})")

    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc, emergency=True))
    time.sleep(0.3)
    ga, gb = A.drain_floor(), B.drain_floor()
    check("GRANT" in ops(gb), f"긴급 B 동시 GRANT (B={ops(gb)})")
    check("REVOKE" not in ops(ga), f"기존 화자 A 는 REVOKE 없음 (A={ops(ga)})")
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g2]
    holders = grp[0].get("floor_holders") if grp else None
    check(holders and sorted(holders) == ["A", "B"], f"STATS 동시 발언자 2명 ({holders})")
    remove(g2)
    A.close(); B.close()

    # ── 3. multi-talker (max_talkers=3) ──────────────────────────────────
    print("\n[3] multi — 동시 3명 GRANT, 4번째는 큐, 1명 해제 시 IDLE 대신 잔여 TAKEN")
    g3 = f"{a.prefix}_multi"
    ms = [Member(x, bp + 20 + 4 * i, 0x3001 + i) for i, x in enumerate(["A", "B", "C", "D"])]
    r = add_group(g3, members="A:5:participant,B:5:participant,C:5:participant,D:5:participant",
                  floor_policy="multi", max_talkers=3)
    fp = payload(r).get("floor_port")
    check(status(r) == "OK", "ADD multi ok")
    for m in ms:
        join(g3, m)
        m.drain_floor()
    for m in ms[:3]:
        m.send_floor(a.cmp, fp, req_pkt(m.sid, m.ssrc))
        time.sleep(0.2)
    raw = {m.sid: m.drain_floor() for m in ms}
    got = {k: ops(v) for k, v in raw.items()}
    check(all("GRANT" in got[s] for s in "ABC"), f"A/B/C 동시 GRANT ({ {k: v for k, v in got.items()} })")
    # 동시 발언 Taken 은 화자 전원을 리스트로 싣는다 (§8.2.9 / §6.3.4.4.7a-3c)
    tk = [m for m in raw["D"] if m["op_base"] == TAKEN]
    if tk:
        last = tk[-1]
        users = user_list(last["fields"].get(FF_GRANTED_USERS, b""))
        check(sorted(users) == ["A", "B", "C"], f"TAKEN List of Granted Users ({users})")
        check(len(ssrc_list(last["fields"].get(FF_SSRC_LIST, b""))) == 3, "TAKEN List of SSRCs 3개")
        check(struct.unpack(">H", last["fields"][FF_INDICATOR])[0] & FI_MULTI, "TAKEN Multi-talker 비트")
    else:
        check(False, "동시 발언 TAKEN 수신")
    ms[3].send_floor(a.cmp, fp, req_pkt("D", ms[3].ssrc))
    time.sleep(0.3)
    gd = ops(ms[3].drain_floor())
    check("QPOS_INFO" in gd and "GRANT" not in gd, f"정원 초과 D 는 큐 (D={gd})")
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g3]
    holders = sorted(grp[0].get("floor_holders") or []) if grp else []
    check(holders == ["A", "B", "C"], f"STATS floor_holders 3명 ({holders})")

    ctl.events.clear()
    ms[0].send_floor(a.cmp, fp, rel_pkt("A", ms[0].ssrc))   # 화자 1명 해제
    time.sleep(0.4)
    gb = ms[1].drain_floor()
    check("IDLE" not in ops(gb), f"잔여 화자 있으면 IDLE 아님 (B={ops(gb)})")
    # 규격: 나머지 참가자에게 Floor Release Multi Talker(0x0F) 로 알린다 (§8.2.14)
    rm = [m for m in gb if m["op_base"] == RELEASE_MULTI]
    check(bool(rm), f"잔여 화자에게 RELEASE_MULTI 통지 (B={ops(gb)})")
    if rm:
        check(rm[0]["fields"].get(FF_USER_ID, b"").decode(errors="ignore").rstrip("\x00") == "A",
              "RELEASE_MULTI User ID = 해제한 화자")
        check(struct.unpack(">I", rm[0]["fields"].get(FF_SSRC, b"\0\0\0\0")[:4])[0] == ms[0].ssrc,
              "RELEASE_MULTI SSRC 필드 = 해제한 화자 SSRC")
    check("RELEASE_MULTI" not in ops(ms[0].drain_floor()), "해제한 화자에게는 미송신")
    check("GRANT" in ops(ms[3].drain_floor()), "여유 정원으로 대기자 D 승급")
    evs = [e for e in ctl.drain_events(0.5) if (e.get("hdr") or {}).get("cmd") == "FLOOR_TALKERS"]
    check(bool(evs), f"FLOOR_TALKERS 이벤트 수신 ({len(evs)}건)")
    if evs:
        last = evs[-1].get("payload") or {}
        check(last.get("policy") == "multi" and isinstance(last.get("talkers"), list),
              f"이벤트 payload policy/talkers ({last})")
    # 0x0F 는 서버→단말 통지 전용(§8.2.14) — 단말이 보내면 무시한다(발언 해제는 Floor Release).
    ms[3].send_floor(a.cmp, fp, rel_pkt("D", ms[3].ssrc, multi=True))
    time.sleep(0.35)
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g3]
    holders = sorted(grp[0].get("floor_holders") or []) if grp else []
    check("D" in holders, f"단말이 보낸 0x0F 는 무시 — 발언권 유지 ({holders})")
    ms[3].send_floor(a.cmp, fp, rel_pkt("D", ms[3].ssrc))   # 규격대로 Floor Release
    time.sleep(0.35)
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g3]
    holders = sorted(grp[0].get("floor_holders") or []) if grp else []
    check("D" not in holders, f"Floor Release 로는 해제 ({holders})")
    remove(g3)
    for m in ms:
        m.close()

    # ── 4. private call ──────────────────────────────────────────────────
    print("\n[4] private — 개시자 초기 GRANT, 상대 요청은 큐 없이 DENY")
    g4 = f"{a.prefix}_priv"
    A = Member("A", bp + 40, 0x4001)
    B = Member("B", bp + 44, 0x4002)
    r = add_group(g4, members="A:5:participant,B:5:participant",
                  group_type="private", initiator_id="A")
    fp = payload(r).get("floor_port")
    check(status(r) == "OK" and fp, "ADD private(with floor) ok")
    join(g4, A); join(g4, B)
    time.sleep(0.3)
    ga = A.drain_floor()
    check("GRANT" in ops(ga), f"개시자 A 초기 발언권 (A={ops(ga)})")
    B.drain_floor()
    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))
    time.sleep(0.3)
    gb = ops(B.drain_floor())
    check("DENY" in gb and "QPOS_INFO" not in gb, f"private 은 큐 없이 DENY (B={gb})")
    remove(g4)
    A.close(); B.close()

    # ── 5. private without floor (full-duplex) ───────────────────────────
    print("\n[5] floor_control=off — floor_port 미광고, floor 무시, 양방향 미디어 중계")
    g5 = f"{a.prefix}_nofloor"
    A = Member("A", bp + 50, 0x5001)
    B = Member("B", bp + 54, 0x5002)
    r = add_group(g5, members="A:5:participant,B:5:participant",
                  group_type="private", floor_control="off", initiator_id="A")
    check(status(r) == "OK" and "floor_port" not in payload(r), f"floor_port 미광고 ({list(payload(r).keys())})")
    pa = payload(join(g5, A)).get("port")
    pb = payload(join(g5, B)).get("port")
    check(bool(pa and pb), f"멤버 포트 {pa}/{pb}")
    A.drain_rtp(); B.drain_rtp()
    for i in range(3):
        A.send_rtp(a.cmp, pa, seq=i + 1)
        B.send_rtp(a.cmp, pb, seq=i + 1)
        time.sleep(0.05)
    time.sleep(0.3)
    na, nb = A.drain_rtp(), B.drain_rtp()
    check(na >= 2 and nb >= 2, f"floor 없이 양방향 중계 (A수신={na} B수신={nb})")
    remove(g5)
    A.close(); B.close()

    # ── 6. ambient listening (recv_only / floor_suppress) ────────────────
    print("\n[6] ambient — recv_only 상향 미중계 + 발언 DENY, floor_suppress 는 floor 미수신")
    g6 = f"{a.prefix}_ambient"
    A = Member("A", bp + 60, 0x6001)
    L = Member("L", bp + 64, 0x6002)
    r = add_group(g6, members="A:5:participant,L:5:participant")
    fp = payload(r).get("floor_port")
    pa = payload(join(g6, A)).get("port")
    pl = payload(join(g6, L, recv_only=1, floor_suppress=1)).get("port")
    A.drain_floor(); L.drain_floor(); A.drain_rtp(); L.drain_rtp()

    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.3)
    check("GRANT" in ops(A.drain_floor()), "A GRANT")
    check(not ops(L.drain_floor()), "floor_suppress 청취자에게 floor 메시지 미송신")
    for i in range(3):
        A.send_rtp(a.cmp, pa, seq=i + 1)
        L.send_rtp(a.cmp, pl, seq=i + 1)
        time.sleep(0.05)
    time.sleep(0.3)
    check(L.drain_rtp() >= 2, "청취자는 하향 미디어 수신")
    check(A.drain_rtp() == 0, "recv_only 상향은 중계 안 됨")
    L.send_floor(a.cmp, fp, req_pkt("L", L.ssrc))
    time.sleep(0.3)
    check(not ops(L.drain_floor()), "청취자 발언 요청 응답도 억제(DENY 미송신)")
    remove(g6)
    A.close(); L.close()

    # ── 7. floor_crypto (SRTCP) ──────────────────────────────────────────
    print("\n[7] floor_crypto — SRTCP 왕복, 평문 요청은 거부(floor_crypto_drop 증가)")
    if not HAVE_CRYPTO:
        print("    (python cryptography 미설치 — SKIP)")
    else:
        g7 = f"{a.prefix}_crypto"
        key = bytes.fromhex("E1F97A0D3E018BE0D64FA32C06DE4139")
        salt = bytes.fromhex("0EC675AD498AFEEBB6960B3AABE6")
        A = Member("A", bp + 70, 0x7001, crypto=Srtcp(key, salt))
        B = Member("B", bp + 74, 0x7002, crypto=Srtcp(key, salt))
        r = add_group(g7, members="A:5:participant,B:5:participant",
                      floor_crypto={"alg": "AES_CM_128_HMAC_SHA1_80",
                                    "key": base64.b64encode(key).decode(),
                                    "salt": base64.b64encode(salt).decode()})
        fp = payload(r).get("floor_port")
        check(status(r) == "OK", f"ADD floor_crypto ok ({status(r)} {code(r)})")
        join(g7, A); join(g7, B)
        A.drain_floor(); B.drain_floor()
        st0 = payload(ctl.send("STATS")).get("detail", {}).get("floor_crypto_drop", 0)

        A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
        time.sleep(0.3)
        ga, gb = A.drain_floor(), B.drain_floor()
        check("GRANT" in ops(ga), f"SRTCP 요청 수용 + 보호된 GRANT 복호 (A={ops(ga)})")
        check("TAKEN" in ops(gb), f"보호된 TAKEN 브로드캐스트 (B={ops(gb)})")

        B.fsock.sendto(req_pkt("B", B.ssrc), (a.cmp, fp))    # 평문(비보호) 요청
        time.sleep(0.3)
        gb = B.drain_floor()
        check(not [m for m in gb if m.get("op") == "GRANT"], f"평문 요청 무시 (B={ops(gb)})")
        st1 = payload(ctl.send("STATS")).get("detail", {}).get("floor_crypto_drop", 0)
        check(st1 > st0, f"floor_crypto_drop 증가 ({st0} → {st1})")
        remove(g7)
        A.close(); B.close()

    # ── 8. 계약 위반 거절 ────────────────────────────────────────────────
    print("\n[8] 계약 검증 — 잘못된 정책/키는 BAD_REQUEST")
    bad = [
        ("floor_policy 오타", dict(floor_policy="dual2")),
        ("multi + max_talkers 누락", dict(floor_policy="multi")),
        ("max_talkers 상한 초과", dict(floor_policy="multi", max_talkers=9)),
        ("floor_control 오타", dict(floor_control="none")),
        ("crypto 키 길이", dict(floor_crypto={"key": base64.b64encode(b"short").decode(),
                                              "salt": base64.b64encode(b"x" * 14).decode()})),
        ("floor off + crypto", dict(floor_control="off",
                                    floor_crypto={"key": base64.b64encode(b"k" * 16).decode(),
                                                  "salt": base64.b64encode(b"s" * 14).decode()})),
    ]
    for i, (label, kw) in enumerate(bad):
        gid = f"{a.prefix}_bad{i}"
        r = add_group(gid, members="A:5:participant", **kw)
        check(code(r) == "BAD_REQUEST", f"{label} → {status(r)} {code(r)}")
        remove(gid)

    # ── 9. 정책 변경(MODIFY) 일관성 ──────────────────────────────────────
    print("\n[9] MODIFY 정책 변경 — dual 큐는 override 자리 미충원, 정원 축소 시 초과 화자 회수")
    g9 = f"{a.prefix}_modify"
    A = Member("A", bp + 80, 0x9001)
    B = Member("B", bp + 84, 0x9002)
    C = Member("C", bp + 88, 0x9003)
    r = add_group(g9, members="A:5:participant,B:5:participant,C:5:participant",
                  floor_policy="multi", max_talkers=3)
    fp = payload(r).get("floor_port")
    for m in (A, B, C):
        join(g9, m)
        m.drain_floor()
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g9]
    check(grp and grp[0].get("floor_policy") == "multi", f"STATS floor_policy 노출 ({grp and grp[0].get('floor_policy')})")

    for m in (A, B, C):
        m.send_floor(a.cmp, fp, req_pkt(m.sid, m.ssrc))
        time.sleep(0.2)
    for m in (A, B, C):
        m.drain_floor()
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g9]
    check(sorted(grp[0].get("floor_holders") or []) == ["A", "B", "C"], "3명 동시 발언 준비")

    # multi(3) → single 로 축소: 초과 화자 2명은 REVOKE 되어야 한다
    r = ctl.send("PTT_GROUP_MODIFY", group_id=g9, sesid=f"probe_{g9}",
                 members="A:5:participant,B:5:participant,C:5:participant", floor_policy="single")
    check(status(r) == "OK", f"MODIFY single ({status(r)} {code(r)})")
    time.sleep(0.4)
    revoked = sum(1 for m in (A, B, C) if "REVOKE" in ops(m.drain_floor()))
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g9]
    holders = grp[0].get("floor_holders") or [] if grp else []
    check(holders == ["A"], f"정원 축소 후 최초 화자 A 만 유지 ({holders})")
    check(revoked == 2, f"초과 화자 2명 REVOKE 수신 (실제 {revoked})")
    check(grp and grp[0].get("floor_policy") == "single", "STATS 정책 갱신")
    remove(g9)
    A.close(); B.close(); C.close()

    # ── 11. floor 타이머 (T1/T2/T3/T8) + 재요청/큐 위치 유지 ────────────────
    print("\n[11] 타이머 — T1 발언종료·T2 발언시간 초과·T3 회수 유예·T8 재전송")
    g11 = f"{a.prefix}_timer"
    A = Member("A", bp + 106, 0xB001)
    B = Member("B", bp + 110, 0xB002)
    r = add_group(g11, members="A:5:participant,B:9:participant",
                  floor_timers={"t1_end_rtp": 2, "t2_stop_talk": 3, "t3_grace": 2, "t8_revoke": 1})
    fp = payload(r).get("floor_port")
    check(status(r) == "OK" and fp, f"ADD floor_timers ok ({status(r)} {code(r)})")
    ports = payload(r).get("member_ports") or {}
    for m in (A, B):
        join(g11, m)
        m.drain_floor()

    # T1 — RTP 없이 grant 후 2초 무수신 = 발언 종료. REVOKE 없이 IDLE.
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.3)
    ga = A.drain_floor()
    grant = [m for m in ga if m["op_base"] == GRANT]
    check(bool(grant), f"A GRANT (A={ops(ga)})")
    if grant:
        check(struct.unpack(">H", grant[0]["fields"][FF_DURATION])[0] == 3,
              "GRANT Duration = T2(3초) — 최대 발언시간 광고")
    B.drain_floor()
    time.sleep(3.0)
    gb, ga = B.drain_floor(), A.drain_floor()
    check("IDLE" in ops(gb), f"T1 만료로 발언 종료 → IDLE (B={ops(gb)})")
    check("REVOKE" not in ops(ga), f"T1 만료에는 REVOKE 를 보내지 않는다 (A={ops(ga)})")

    # T2 — RTP 를 계속 보내며 3초 넘게 발언 → Revoke cause #2(Media burst too long)
    aport = (ports.get("A") or {}).get("port")
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.3); A.drain_floor(); B.drain_floor()
    t_end = time.time() + 4.2
    seq = 1
    while time.time() < t_end:
        A.send_rtp(a.cmp, aport, seq=seq); seq += 1
        time.sleep(0.15)
    time.sleep(2.5)   # T3(2초) 유예 경과 — 그 사이 T8(1초)로 REVOKE 가 재전송된다
    ga = A.drain_floor()
    rev = [m for m in ga if m["op_base"] == REVOKE]
    check(bool(rev), f"T2 초과 → REVOKE (A={ops(ga)})")
    if rev:
        check(struct.unpack(">H", rev[0]["fields"][FF_CAUSE])[0] == 2,
              "REVOKE cause #2 (Media burst too long)")
        check(len(rev) >= 2, f"T8 로 REVOKE 재전송 (수신 {len(rev)}건)")
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g11]
    check(not (grp and grp[0].get("floor_holders")), f"T3 유예 후 회수 완료 ({grp and grp[0].get('floor_holders')})")
    remove(g11)
    A.close(); B.close()

    print("\n[12] 선점 — Revoke 유예(T3) 중 미디어 유지, 요청자는 큐 선두에서 승급")
    g12 = f"{a.prefix}_preempt"
    A = Member("A", bp + 114, 0xC001)
    B = Member("B", bp + 118, 0xC002)
    r = add_group(g12, members="A:3:participant,B:9:participant",
                  floor_timers={"t1_end_rtp": 6, "t2_stop_talk": 0, "t3_grace": 3, "t8_revoke": 1})
    fp = payload(r).get("floor_port")
    for m in (A, B):
        join(g12, m)
        m.drain_floor()
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc, prio=3))
    time.sleep(0.3); A.drain_floor(); B.drain_floor()

    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc, prio=9))     # 상위 우선순위 선점
    time.sleep(0.4)
    ga, gb = A.drain_floor(), B.drain_floor()
    check("REVOKE" in ops(ga), f"기존 화자 A 에 REVOKE (A={ops(ga)})")
    check("QPOS_INFO" in ops(gb) and "GRANT" not in ops(gb),
          f"선점 요청자는 즉시 GRANT 가 아니라 큐 선두 대기 (B={ops(gb)})")
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g12]
    check(grp and grp[0].get("floor_holders") == ["A"],
          f"유예 중에는 기존 화자가 floor 유지 ({grp and grp[0].get('floor_holders')})")

    A.send_floor(a.cmp, fp, rel_pkt("A", A.ssrc))             # 회수 응답 → 승급
    time.sleep(0.4)
    check("GRANT" in ops(B.drain_floor()), "revoked 화자의 RELEASE 후 선점 요청자 GRANT")

    # T3 만료 경로 — 이번엔 RELEASE 를 보내지 않는다
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc, prio=3))     # A 는 큐(하위 우선순위)
    time.sleep(0.3); A.drain_floor()
    B.send_floor(a.cmp, fp, rel_pkt("B", B.ssrc))             # B 해제 → A 승급
    time.sleep(0.4); A.drain_floor(); B.drain_floor()
    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc, prio=9))     # B 재선점
    time.sleep(4.0)                                            # RELEASE 없이 T3(3초) 경과
    gb = B.drain_floor()
    check("GRANT" in ops(gb), f"T3 만료 후 선점 요청자 자동 GRANT (B={ops(gb)})")
    remove(g12)
    A.close(); B.close()

    print("\n[13] 재요청/큐 위치 — 화자 재요청은 GRANT 재송신, 대기자 재요청은 위치 유지")
    g13 = f"{a.prefix}_requeue"
    A = Member("A", bp + 122, 0xD001)
    B = Member("B", bp + 126, 0xD002)
    C = Member("C", bp + 130, 0xD003)
    r = add_group(g13, members="A:5:participant,B:5:participant,C:5:participant",
                  floor_timers={"t1_end_rtp": 20, "t2_stop_talk": 0, "t3_grace": 3, "t8_revoke": 1})
    fp = payload(r).get("floor_port")
    for m in (A, B, C):
        join(g13, m)
        m.drain_floor()
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))
    time.sleep(0.3); A.drain_floor()
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))              # 화자 재요청(§6.3.4.4.8)
    time.sleep(0.3)
    check("GRANT" in ops(A.drain_floor()), "발언 중 재요청 → GRANT 재송신")

    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))              # 큐 1번
    time.sleep(0.25)
    C.send_floor(a.cmp, fp, req_pkt("C", C.ssrc))              # 큐 2번
    time.sleep(0.25); B.drain_floor(); C.drain_floor()
    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))              # B 재전송 — 위치 유지여야
    time.sleep(0.3)
    gb = B.drain_floor()
    qp = [m for m in gb if m["op_base"] == QUEUE_POS_INFO]
    check(bool(qp), f"대기자 재요청 → QPOS_INFO 재회신 (B={ops(gb)})")
    if qp:
        check(qp[-1]["fields"][FF_QUEUE_INFO][0] == 1, f"큐 위치 유지 (pos={qp[-1]['fields'][FF_QUEUE_INFO][0]})")
    A.send_floor(a.cmp, fp, rel_pkt("A", A.ssrc))
    time.sleep(0.4)
    check("GRANT" in ops(B.drain_floor()), "먼저 대기한 B 가 승급")
    remove(g13)
    A.close(); B.close(); C.close()

    print("\n[10] dual — 큐 대기자는 override 자리를 채우지 않는다")
    g10 = f"{a.prefix}_dualq"
    A = Member("A", bp + 92, 0xA001)
    B = Member("B", bp + 96, 0xA002)
    C = Member("C", bp + 100, 0xA003)
    r = add_group(g10, members="A:5:participant,B:5:participant,C:9:participant", floor_policy="dual")
    fp = payload(r).get("floor_port")
    for m in (A, B, C):
        join(g10, m)
        m.drain_floor()
    A.send_floor(a.cmp, fp, req_pkt("A", A.ssrc))          # A 발언
    time.sleep(0.25)
    B.send_floor(a.cmp, fp, req_pkt("B", B.ssrc))          # B 동급 → 큐
    time.sleep(0.25)
    C.send_floor(a.cmp, fp, req_pkt("C", C.ssrc, emergency=True))   # C 긴급 → dual grant
    time.sleep(0.35)
    for m in (A, B, C):
        m.drain_floor()
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g10]
    check(sorted(grp[0].get("floor_holders") or []) == ["A", "C"], f"dual 2명 ({grp[0].get('floor_holders')})")
    C.send_floor(a.cmp, fp, rel_pkt("C", C.ssrc))          # 긴급 화자 해제 → 자리 1개 여유
    time.sleep(0.4)
    gb = ops(B.drain_floor())
    check("GRANT" not in gb, f"대기자 B 는 override 자리로 승급하지 않음 (B={gb})")
    A.send_floor(a.cmp, fp, rel_pkt("A", A.ssrc))          # 마지막 화자 해제 → 승급
    time.sleep(0.4)
    check("GRANT" in ops(B.drain_floor()), "발언자가 모두 빠지면 대기자 승급")
    remove(g10)
    A.close(); B.close(); C.close()

    print(f"\n결과: {len(PASS)} passed, {len(FAIL)} failed")
    for f in FAIL:
        print(f"  FAIL: {f}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
