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
FF_PRIORITY, FF_GRANTED_PARTY, FF_USER_ID, FF_INDICATOR = 0, 4, 6, 13
FI_EMERGENCY, FI_DUAL, FI_MULTI = 0x1000, 0x0200, 0x0080

PASS, FAIL = [], []


def check(cond, msg):
    (PASS if cond else FAIL).append(msg)
    print(f"    {'✓' if cond else '✗'} {msg}")


# ── floor 코덱 (cmp/PFloorCodec.cpp 와 동일 규약) ────────────────────────────
def _pad4(n):
    return (4 - (n % 4)) % 4


def floor_build(subtype, ssrc, fields):
    body = b""
    for fid, val in fields:
        body += bytes([fid, len(val)]) + val
        if fid in (FF_GRANTED_PARTY, FF_USER_ID):
            body += b"\x00" * _pad4(2 + len(val))
    body += b"\x00" * _pad4(len(body))
    total = 12 + len(body)
    return struct.pack(">BBHI4s", 0x80 | (subtype & 0x1F), RTCP_PT_APP, total // 4 - 1,
                       ssrc, b"MCPT") + body


def floor_parse(buf):
    if len(buf) < 12 or buf[8:12] != b"MCPT":
        return None
    out = {"subtype": buf[0] & 0x1F, "ssrc": struct.unpack(">I", buf[4:8])[0], "fields": {}}
    p = 12
    while p + 2 <= len(buf):
        fid, ln = buf[p], buf[p + 1]
        start = p
        p += 2
        if p + ln > len(buf):
            break
        val = buf[p:p + ln]
        p += ln
        if fid in (FF_GRANTED_PARTY, FF_USER_ID):
            p += _pad4(p - start)
        if fid == 0 and ln == 0:
            break
        out["fields"][fid] = val
    out["op"] = OPN.get(out["subtype"], f"?{out['subtype']}")
    return out


def req_pkt(user, ssrc, prio=5, emergency=False):
    f = [(FF_PRIORITY, bytes([prio, 0])), (FF_USER_ID, user.encode())]
    if emergency:
        f.append((FF_INDICATOR, struct.pack(">H", FI_EMERGENCY)))
    return floor_build(REQUEST, ssrc, f)


def rel_pkt(user, ssrc, multi=False):
    return floor_build(RELEASE_MULTI if multi else RELEASE, ssrc, [(FF_USER_ID, user.encode())])


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
                    got.append({"op": "AUTH_FAIL", "fields": {}})
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
            for m in msgs if m["subtype"] == op]


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
    B.send_floor(a.cmp, fp, rel_pkt("B", B.ssrc))
    time.sleep(0.3)
    check("IDLE" in ops(A.drain_floor()), "마지막 해제 후 IDLE")
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
    got = {m.sid: ops(m.drain_floor()) for m in ms}
    check(all("GRANT" in got[s] for s in "ABC"), f"A/B/C 동시 GRANT ({ {k: v for k, v in got.items()} })")
    ms[3].send_floor(a.cmp, fp, req_pkt("D", ms[3].ssrc))
    time.sleep(0.3)
    gd = ops(ms[3].drain_floor())
    check("QPOS_INFO" in gd and "GRANT" not in gd, f"정원 초과 D 는 큐 (D={gd})")
    st = payload(ctl.send("STATS"))
    grp = [g for g in (st.get("detail") or {}).get("groups", []) if g.get("group_id") == g3]
    holders = sorted(grp[0].get("floor_holders") or []) if grp else []
    check(holders == ["A", "B", "C"], f"STATS floor_holders 3명 ({holders})")

    ctl.events.clear()
    ms[0].send_floor(a.cmp, fp, rel_pkt("A", ms[0].ssrc, multi=True))   # Floor Release Multi Talker
    time.sleep(0.4)
    gb = ms[1].drain_floor()
    check("IDLE" not in ops(gb), f"잔여 화자 있으면 IDLE 아님 (B={ops(gb)})")
    check("TAKEN" in ops(gb), f"잔여 화자 TAKEN 갱신 (B={ops(gb)})")
    check("GRANT" in ops(ms[3].drain_floor()), "여유 정원으로 대기자 D 승급")
    evs = [e for e in ctl.drain_events(0.5) if (e.get("hdr") or {}).get("cmd") == "FLOOR_TALKERS"]
    check(bool(evs), f"FLOOR_TALKERS 이벤트 수신 ({len(evs)}건)")
    if evs:
        last = evs[-1].get("payload") or {}
        check(last.get("policy") == "multi" and isinstance(last.get("talkers"), list),
              f"이벤트 payload policy/talkers ({last})")
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
