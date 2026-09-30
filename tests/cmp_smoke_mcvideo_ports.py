#!/usr/bin/env python3
# CMP MCVideo 그룹 호 스모크 — cmp_media_api.md §7.9 (계약 K6) · mcvideo_dev_plan.md B3
#   HEARTBEAT resource.mcvideo → ADD 거절(max_transmitters 없음·floor 필드·group_type·tc_timers 범위) → ADD chat g101(멤버 포트 6포트 블록)
#   → 같은 id 의 MCPTT 그룹과 동시(자원 키 (service, group_id)) → JOIN ① 선할당(tc_ssrc 멱등) ② 주소 등록 → 로스터 밖 멤버·풀 소진 NO_RESOURCE
#   → implicit_request(chat 거절·prearranged granted 0) → 보호 키 거절 → PTT_FLOOR_TIER 거절 → 수신 판정(선언 소스 = 허가 없는 미디어 드롭 ·
#   전송 제어 MCV0 수신 · 미선언 소스 드롭 · 주소 전 드롭) → STATS mcvideo_groups → MCVideo 해제가 MCPTT 그룹에 무영향 → 멱등 LEAVE/REMOVE
#   → MODIFY NOT_FOUND → 자원 0 복귀
#   → 전송 제어 흐름(B4·B5 — PMcvControl): 참가 Idle(헤더 SSRC = user_tc_ssrc) → Transmission Request → Granted(송출 SSRC = offer a=ssrc)·
#   Notification·TRANSMITTERS 이벤트 → manual 수신 전 영상 미분배 → Receive Media Request → Response(ack 비트)·Ack → 영상·음성 분배(SSRC =
#   송출 할당값 · PT = 수신 leg 값) · 받지 않는 멤버 미분배 → 헤더만 RTP 무시 · 무허가 미디어 Revoked #3 → 상한 Rejected #1 → STATS
#   transmitters·receptions → End Request → End Response·End Notify·Idle·TRANSMITTERS [] → T1 만료 TRANSMISSION_INACTIVITY 이벤트
#   → 키프레임 요청(B6): 수신 시작 → 송출자에게 PLI(RR + SDES CNAME + PSFB, media source = 송출자 원래 영상 SSRC) · 수신자 PLI(할당
#   SSRC)·FIR 전달(SSRC 되돌림 · FIR Seq nr = CMP 몫 · 간격 제한 하나로) · 받지 않는 멤버·없는 송출의 PLI 는 버림 · STATS keyframe_requests
#   → 보호(B7): tc_crypto 형식 거절 · floor_crypto 거절 → 그룹 키·멤버 CSK SRTCP(Idle·Granted 를 받는 쪽 키로 풀고, 평문·다른 키 요청은
#   crypto_drop) · 멤버 SRTP(상향 = 멤버 rx 키로 풀고 하향 = 받는 멤버 tx 키로 보호 — SSRC·PT 찍기 뒤) · 틀린 키 영상 버림 —
#   SRTP/SRTCP 는 이 파일의 파이썬 구현(RFC 3711, cryptography)으로 CMP 와 교차 확인
# 사용법: python3 tests/cmp_smoke_mcvideo_ports.py [--cmp build/bin/cmp]     — 시험용 CMP 를 빈 포트 창에 직접 띄우고 끝나면 내린다
#         python3 tests/cmp_smoke_mcvideo_ports.py --target IP:PORT            — 이미 띄운 시험용 CMP(McVideoMemberPoolSize 4, 같은 호스트)
#   **라이브 CMP 에 돌리지 않는다** — 이벤트 회신처가 마지막 요청 소켓이라 CSP 의 이벤트를 가로챈다.
import argparse, base64, hashlib, hmac, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--cmp", default=os.path.join(ROOT, "build", "bin", "cmp"))
ap.add_argument("--target", default="")
args = ap.parse_args()

IP = "127.0.0.1"
POOL = 4


def free_window(n=60, lo=33000, hi=60000, step=100):
    """n 포트가 모두 비어 있는 창의 시작 포트 (UDP bind 로 확인)."""
    for base in range(lo, hi, step):
        socks, ok = [], True
        try:
            for p in range(base, base + n):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.bind((IP, p))
                socks.append(s)
        except OSError:
            ok = False
        for s in socks:
            s.close()
        if ok:
            return base
    raise SystemExit("free port window not found")


proc, tmp = None, None
if args.target:
    h, p = args.target.rsplit(":", 1)
    CMP = (h, int(p))
else:
    if not os.access(args.cmp, os.X_OK):
        raise SystemExit(f"cmp binary not found: {args.cmp} (make cmp)")
    B = free_window()
    tmp = tempfile.mkdtemp(prefix="cmp_smoke_mcv_")
    cfg = {"ServerIp": IP, "ServerPort": B + 50, "RtpIp": IP,
           "RtpStartPort": B, "RtpPoolSize": 1, "TranscodeSlots": 0, "TapPoolSize": 0, "AnnPlayers": 0,
           "PttRtpStartPort": B + 10, "PttMemberPoolSize": 1, "PttVideoStartPort": B + 12,
           "PttFloorStartPort": B + 14, "PttRtpPoolSize": 1, "PttMediaBufferMs": 0,
           "McVideoStartPort": B + 20, "McVideoMemberPoolSize": POOL,
           "RtpWorkerCount": 1, "LogDir": tmp, "SystemId": "cmp_smoke",
           "ServiceLogging": {"Dir": ""}, "Fm": {"Enable": False}}
    with open(os.path.join(tmp, "cmp.json"), "w") as f:
        json.dump(cfg, f)
    CMP = (IP, B + 50)
    proc = subprocess.Popen([args.cmp, os.path.join(tmp, "cmp.json")], cwd=tmp,
                            stdout=open(os.path.join(tmp, "stdout.log"), "w"), stderr=subprocess.STDOUT)

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ctrl.bind((IP, 0))
_tid = [7920000]
results = []
events = []   # CMP → client 이벤트 (req() 가 ack 하며 모은다)


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


def req(cmd, payload, service="mcvideo", sesid="mcv-smoke::1", timeout=3.0):
    _tid[0] += 1
    msg = {"hdr": {"ver": 2, "trans_id": _tid[0], "node": "csp-smoke", "cmd": cmd, "type": "request",
                   "sesid": sesid, "service": service}, "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), CMP)
    ctrl.settimeout(timeout)
    while True:
        d, _ = ctrl.recvfrom(8192)
        r = json.loads(d.decode())
        if r["hdr"].get("type") == "event":
            events.append(r)
            ctrl.sendto(json.dumps({"hdr": {"ver": 2, "trans_id": r["hdr"]["trans_id"], "node": "csp-smoke",
                                            "cmd": r["hdr"]["cmd"], "type": "response", "status": "OK"}}).encode(), CMP)
            continue
        if r["hdr"].get("trans_id") == _tid[0]:
            return r


def st(r):
    return r["hdr"].get("status"), r["hdr"].get("code", "")


def pl(r):
    return r.get("payload", {}) or {}


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((IP, 0))
    return s


def udp_pair():
    """영상 RTP·RTCP 소켓 쌍 — RTCP = RTP + 1 (a=rtcp 없음, RFC 3550 §11)"""
    for _ in range(200):
        a = udp()
        b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            b.bind((IP, a.getsockname()[1] + 1))
            return a, b
        except OSError:
            a.close()
            b.close()
    raise SystemExit("no consecutive UDP port pair")


def parse_rtcp(d):
    """compound RTCP → [{pt, fmt, ssrc(packet sender), media, fci(bytes)}]"""
    out, off = [], 0
    while off + 4 <= len(d) and d[off] >> 6 == 2:
        plen = (struct.unpack("!H", d[off + 2:off + 4])[0] + 1) * 4
        pk = d[off:off + plen]
        e = {"pt": pk[1], "fmt": pk[0] & 0x1F, "ssrc": struct.unpack("!I", pk[4:8])[0] if plen >= 8 else 0}
        if pk[1] in (205, 206) and plen >= 12:
            e["media"] = struct.unpack("!I", pk[8:12])[0]
            e["fci"] = pk[12:]
        if pk[1] == 202 and plen >= 12:
            e["cname"] = pk[10:10 + pk[9]].decode(errors="replace") if pk[8] == 1 else ""
        out.append(e)
        off += plen
    return out


def psfb(ps, fmt):
    return [e for e in ps if e["pt"] == 206 and e["fmt"] == fmt]


def rtp(pt=96, ssrc=0x11110001, seq=1):
    return struct.pack("!BBHII", 0x80, pt, seq, 1000, ssrc) + b"\0" * 20


def mcv0(subtype, ssrc):
    return bytes([0x80 | subtype, 204]) + struct.pack("!H", 2) + struct.pack("!I", ssrc) + b"MCV0"


def tlv(fid, value):
    b = bytes([fid, len(value)]) + value
    return b + b"\0" * ((4 - len(b) % 4) % 4)


def app(name, subtype, ssrc, fields=b""):
    words = (12 + len(fields)) // 4 - 1
    return bytes([0x80 | subtype, 204]) + struct.pack("!HI", words, ssrc) + name + fields


def parse_app(d):
    """RTCP APP 전송 제어 메시지 → {name, subtype(5비트), ack, ssrc, f{field id: value}}"""
    if len(d) < 12 or d[1] != 204:
        return None
    m = {"name": d[8:12].decode(), "subtype": d[0] & 0x0F, "ack": bool(d[0] & 0x10), "ssrc": struct.unpack("!I", d[4:8])[0],
         "f": {}}
    i = 12
    while i + 2 <= len(d):
        fid, flen = d[i], d[i + 1]
        m["f"][fid] = d[i + 2:i + 2 + flen]
        i += 2 + flen + (4 - (2 + flen) % 4) % 4
    return m


def drain(sock, t=0.4):
    out, end = [], time.time() + t
    while True:
        left = end - time.time()
        if left <= 0:
            return out
        sock.settimeout(left)
        try:
            d, _ = sock.recvfrom(4096)
            out.append(d)
        except socket.timeout:
            return out


def msgs(sock, t=0.4):
    return [m for m in (parse_app(d) for d in drain(sock, t)) if m]


def has(ms, name, subtype):
    return [m for m in ms if m["name"] == name and m["subtype"] == subtype]


def ssrc_f(m, fid):
    v = m["f"].get(fid, b"")
    return struct.unpack("!I", v[:4])[0] if len(v) >= 4 else 0


def u16_f(m, fid):
    v = m["f"].get(fid, b"")
    return struct.unpack("!H", v[:2])[0] if len(v) >= 2 else -1


# ── RFC 3711 SRTP/SRTCP (AES_CM_128_HMAC_SHA1_80, KDR 0) — CMP(libsrtp·PFloorCrypto)와 독립 구현 ──
def _aes_ctr(key, iv, n):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    e = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    return e.update(b"\0" * n) + e.finalize()


def _kdf(mkey, msalt, label, n):
    x = bytearray(msalt)
    x[7] ^= label                      # key_id = label || r(=0), 112비트 오른쪽 정렬(§4.3.1)
    return _aes_ctr(mkey, bytes(x) + b"\0\0", n)


class Srtp:
    def __init__(self, key, salt):
        self.ke, self.ka, self.ks = _kdf(key, salt, 0, 16), _kdf(key, salt, 1, 20), _kdf(key, salt, 2, 14)
        self.ce, self.ca, self.cs = _kdf(key, salt, 3, 16), _kdf(key, salt, 4, 20), _kdf(key, salt, 5, 14)
        self.tx_idx = 0

    @staticmethod
    def _iv(salt, ssrc, idx48):
        iv = bytearray(salt + b"\0\0")
        for i, b in enumerate(struct.pack("!I", ssrc)):
            iv[4 + i] ^= b
        for i, b in enumerate(idx48.to_bytes(6, "big")):
            iv[8 + i] ^= b
        return bytes(iv)

    def _xor(self, key, iv, data):
        ks = _aes_ctr(key, iv, len(data))
        return bytes(a ^ b for a, b in zip(data, ks))

    def protect_rtp(self, pkt, roc=0):
        cc = pkt[0] & 0x0F
        hdr = 12 + 4 * cc
        seq, ssrc = struct.unpack("!H", pkt[2:4])[0], struct.unpack("!I", pkt[8:12])[0]
        body = pkt[:hdr] + self._xor(self.ke, self._iv(self.ks, ssrc, (roc << 16) | seq), pkt[hdr:])
        return body + hmac.new(self.ka, body + struct.pack("!I", roc), hashlib.sha1).digest()[:10]

    def unprotect_rtp(self, pkt, roc=0):
        body, tag = pkt[:-10], pkt[-10:]
        if hmac.new(self.ka, body + struct.pack("!I", roc), hashlib.sha1).digest()[:10] != tag:
            return None
        cc = body[0] & 0x0F
        hdr = 12 + 4 * cc
        seq, ssrc = struct.unpack("!H", body[2:4])[0], struct.unpack("!I", body[8:12])[0]
        return body[:hdr] + self._xor(self.ke, self._iv(self.ks, ssrc, (roc << 16) | seq), body[hdr:])

    def protect_rtcp(self, pkt):
        self.tx_idx += 1
        ssrc = struct.unpack("!I", pkt[4:8])[0]
        enc = pkt[:8] + self._xor(self.ce, self._iv(self.cs, ssrc, self.tx_idx), pkt[8:])
        tail = struct.pack("!I", 0x80000000 | self.tx_idx)
        return enc + tail + hmac.new(self.ca, enc + tail, hashlib.sha1).digest()[:10]

    def unprotect_rtcp(self, pkt):
        if len(pkt) < 8 + 4 + 10:
            return None
        body, tag = pkt[:-10], pkt[-10:]
        if hmac.new(self.ca, body, hashlib.sha1).digest()[:10] != tag:
            return None
        ei = struct.unpack("!I", body[-4:])[0]
        enc, idx = body[:-4], ei & 0x7FFFFFFF
        ssrc = struct.unpack("!I", enc[4:8])[0]
        return enc[:8] + (self._xor(self.ce, self._iv(self.cs, ssrc, idx), enc[8:]) if ei & 0x80000000 else enc[8:])


def b64(b):
    return base64.b64encode(b).decode()


def key_pair(tag):
    return bytes([tag]) * 16, bytes([tag ^ 0x5A]) * 14


def smsgs(sock, ctx, t=0.4):
    """SRTCP 보호 제어 채널 수신 — ctx 로 풀어 해석(풀리지 않으면 None 항목)."""
    out = []
    for d in drain(sock, t):
        pl_ = ctx.unprotect_rtcp(d)
        out.append(parse_app(pl_) if pl_ else None)
    return out


def stats():
    return pl(req("STATS", {}, service="system")).get("detail", {})


def mcv_group(gid):
    for g in stats().get("mcvideo_groups", []):
        if g.get("group_id") == gid:
            return g
    return None


try:
    # CMP 기동 대기 — HEARTBEAT 응답까지
    hb = None
    for _ in range(50):
        try:
            hb = req("HEARTBEAT", {}, service="system", timeout=0.2)
            break
        except (socket.timeout, ConnectionRefusedError, OSError):
            time.sleep(0.1)
    if hb is None:
        raise SystemExit("CMP did not answer HEARTBEAT")
    mcv = pl(hb).get("resource", {}).get("mcvideo")
    check("HEARTBEAT resource.mcvideo", mcv and mcv.get("member_total") == POOL and mcv.get("member_used") == 0, f"{mcv}")

    A, B_, C, D, E = "+82510002001", "+82510002002", "+82510002003", "+82510002004", "+82510002005"
    ROSTER = f"{A}:5:participant,{B_}:3:participant"

    # ── ADD 거절 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER})
    check("ADD without max_transmitters → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "floor_control": "on"})
    check("ADD with floor field → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "group_type": "private"})
    check("ADD group_type private → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "tc_timers": {"c7": 0}})
    check("ADD tc_timers.c7 0 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 17})
    check("ADD max_transmitters 17 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_MODIFY", {"group_id": "g-none", "members": ROSTER, "max_transmitters": 2})
    check("MODIFY unknown group → NOT_FOUND", st(r) == ("ERROR", "NOT_FOUND"), r["hdr"].get("reason"))
    check("rejected ADD leaves no resource", pl(req("HEARTBEAT", {}, service="system"))["resource"]["mcvideo"]["member_used"] == 0)

    # ── ADD chat g101 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "subid": "1", "max_transmitters": 2,
                              "group_type": "chat", "reception_mode": "manual",
                              "tc_timers": {"t1_ms": 30000, "t5_ms": 30000, "c7": 2, "c11": 4}})
    mp = pl(r).get("member_ports", {})
    check("ADD chat g101 OK", st(r)[0] == "OK" and pl(r).get("ip") == IP and "floor_port" not in pl(r), f"{pl(r)}")
    pa, pb = mp.get(A, {}), mp.get(B_, {})
    blocks_ok = all(p and p["video_port"] == p["port"] + 2 and p["control_port"] == p["port"] + 4 for p in (pa, pb))
    check("member_ports = 6-port blocks {port, video_port, control_port}", blocks_ok and pa["port"] != pb["port"]
          and abs(pa["port"] - pb["port"]) % 6 == 0, f"A={pa} B={pb}")

    # ── 같은 group id 의 MCPTT 그룹 — 동시에 선다 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": f"{A}:5:participant", "subid": "1"}, service="mcptt",
            sesid="ptt-smoke::1")
    check("MCPTT g101 alongside MCVideo g101", st(r)[0] == "OK" and pl(r).get("floor_port")
          and pl(r).get("member_ports", {}).get(A, {}).get("port") not in (pa.get("port"), pb.get("port")), f"{pl(r)}")

    # ── JOIN ① 선할당 · 멱등 ──
    r1 = req("PTT_JOIN", {"group_id": "g101", "session_id": A})
    r2 = req("PTT_JOIN", {"group_id": "g101", "session_id": A})
    tc = pl(r1).get("tc_ssrc", 0)
    check("JOIN pre-alloc = roster ports + tc_ssrc", st(r1)[0] == "OK" and pl(r1).get("port") == pa["port"]
          and pl(r1).get("control_port") == pa["control_port"] and tc > 0, f"{pl(r1)}")
    check("JOIN pre-alloc idempotent (same tc_ssrc)", pl(r2).get("tc_ssrc") == tc and pl(r2).get("port") == pa["port"])

    # ── JOIN ② 주소 등록 ──
    ua, uv, uc = udp(), udp(), udp()
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": A, "user_ip": IP, "user_port": ua.getsockname()[1],
                         "user_video_port": uv.getsockname()[1], "user_control_port": uc.getsockname()[1],
                         "user_uri": "sip:+82510002001@ptt.cims.example.kr", "user_tc_ssrc": 0x0A0A0A0A,
                         "user_pt": 96, "user_video_pt": 98, "max_priority": 5, "max_rx_streams": 1, "role": "participant"})
    check("JOIN address → same ports + tc_ssrc", st(r)[0] == "OK" and pl(r).get("tc_ssrc") == tc
          and pl(r).get("port") == pa["port"], f"{pl(r)}")
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "max_rx_streams": 0})
    check("JOIN max_rx_streams 0 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "implicit_request": 1})
    check("JOIN implicit_request on chat → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "media_crypto": {"alg": "AES_CM_128_HMAC_SHA1_80", "rx_key": "x", "tx_key": "y"}})
    check("JOIN media_crypto → BAD_REQUEST (fail-fast)", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g-none", "session_id": A})
    check("JOIN unknown group → NOT_FOUND", st(r) == ("ERROR", "NOT_FOUND"))

    # ── 로스터 밖 멤버 · 풀 소진 ──
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": C})
    check("JOIN off-roster member → new unit", st(r)[0] == "OK" and pl(r).get("port") not in (pa["port"], pb["port"]))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": D})
    check("JOIN 4th member (pool 4)", st(r)[0] == "OK")
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": E})
    check("JOIN 5th member → NO_RESOURCE", st(r) == ("ERROR", "NO_RESOURCE"), r["hdr"].get("reason"))
    r = req("PTT_LEAVE", {"group_id": "g101", "session_id": D})
    check("LEAVE frees unit", st(r)[0] == "OK" and pl(req("HEARTBEAT", {}, service="system"))["resource"]["mcvideo"]["member_used"] == 3)

    # ── prearranged + 암묵적 송출 요청 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g103", "members": f"{E}:5:participant", "max_transmitters": 1,
                              "group_type": "prearranged"}, sesid="mcv-smoke::2")
    check("ADD prearranged g103", st(r)[0] == "OK" and E in pl(r).get("member_ports", {}))
    r = req("PTT_JOIN", {"group_id": "g103", "session_id": E, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "implicit_request": 1}, sesid="mcv-smoke::2")
    check("JOIN implicit_request (prearranged, alone) → granted 0 + reserved SSRC pair (§14.3.7)",
          st(r)[0] == "OK" and pl(r).get("granted") == 0 and pl(r).get("audio_ssrc", 0) > 0
          and pl(r).get("video_ssrc", 0) > 0, f"{pl(r)}")
    check("tc_ssrc unique across groups", pl(r).get("tc_ssrc") not in (0, tc))

    # ── floor 명령은 MCVideo 에 없다 ──
    r = req("PTT_FLOOR_TIER", {"group_id": "g101", "session_id": A, "tier": "emergency"})
    check("PTT_FLOOR_TIER mcvideo → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))

    # ── 수신 판정 ──
    before = stats().get("rtp_src_drop", 0)
    for i in range(3):
        ua.sendto(rtp(seq=i + 1), (IP, pa["port"]))
        uv.sendto(rtp(pt=98, seq=i + 1), (IP, pa["video_port"]))
    uc.sendto(mcv0(0, tc), (IP, pa["control_port"]))          # Transmission Request (헤더 = tc_ssrc)
    uc.sendto(mcv0(4, 0x12345678), (IP, pa["control_port"]))  # Receive Media Request (다른 SSRC — 받고 기록)
    rr = struct.pack("!BBHI", 0x80, 201, 1, tc)                 # 빈 RR keepalive (RFC 3550 §6.4.2) — 드롭으로 세지 않는다
    uc.sendto(rr, (IP, pa["control_port"]))
    uc.sendto(rr + mcv0(2, tc), (IP, pa["control_port"]))     # compound RR + Transmission Release → APP 만 푼다
    uc.sendto(mcv0(1, tc), (IP, pa["control_port"]))          # MCV0 subtype 1 = 정의 없음 → 메시지 전체 버림 (§9.1.4 1)
    stranger = udp()
    stranger.sendto(rtp(), (IP, pa["port"]))                   # 미선언 소스 (nat 아님)
    udp().sendto(rtp(), (IP, pb["port"]))                      # 주소 등록 전 멤버 B
    time.sleep(0.3)
    g = mcv_group("g101") or {}
    after = stats().get("rtp_src_drop", 0)
    check("declared media before grant → no_grant_drop", g.get("no_grant_drop", 0) == 6, f"{g}")
    check("control MCV0 decoded (compound split, RR keepalive ignored)", g.get("control_rx", 0) == 3, f"{g}")
    check("undeclared source + pre-join + unknown subtype dropped", after - before == 3, f"src_drop {before} -> {after}")
    check("STATS mcvideo_groups", g.get("members") == 1 and g.get("reserved") == 3 and g.get("group_type") == "chat"
          and g.get("max_transmitters") == 2, f"{g}")

    # ── MCVideo 해제가 같은 id 의 MCPTT 그룹에 무영향 ──
    r = req("PTT_GROUP_REMOVE", {"group_id": "g101"})
    det = stats()
    ptt_ids = [x.get("group_id") for x in det.get("groups", [])]
    mcv_ids = [x.get("group_id") for x in det.get("mcvideo_groups", [])]
    check("REMOVE mcvideo g101 keeps MCPTT g101", st(r)[0] == "OK" and "g101" in ptt_ids and "g101" not in mcv_ids,
          f"ptt={ptt_ids} mcv={mcv_ids}")
    r = req("PTT_GROUP_REMOVE", {"group_id": "g101"})
    check("REMOVE idempotent", st(r)[0] == "OK")
    r = req("PTT_LEAVE", {"group_id": "g101", "session_id": A})
    check("LEAVE on removed group idempotent", st(r)[0] == "OK")
    ua.sendto(rtp(), (IP, pa["port"]))                         # 해제 뒤 늦은 패킷 — 크래시 없이 버린다
    time.sleep(0.1)
    req("PTT_GROUP_REMOVE", {"group_id": "g103"}, sesid="mcv-smoke::2")
    req("PTT_GROUP_REMOVE", {"group_id": "g101"}, service="mcptt", sesid="ptt-smoke::1")
    res = pl(req("HEARTBEAT", {}, service="system"))["resource"]
    check("all released", res["mcvideo"]["member_used"] == 0 and res["mcvideo"]["groups"] == 0
          and res["ptt"]["groups"] == 0, f"{res['mcvideo']} ptt={res['ptt']}")

    # ── 전송 제어 흐름 (B4·B5 — TS 24.581 §6.3.4~§6.3.7) ──
    X, Y, Z = "+82510003001", "+82510003002", "+82510003003"
    UX, UY, UZ = (f"sip:{n}@ptt.cims.example.kr" for n in (X, Y, Z))
    r = req("PTT_GROUP_ADD", {"group_id": "g105", "members": f"{X}:5:participant,{Y}:3:participant,{Z}:1:participant",
                              "max_transmitters": 1, "group_type": "chat", "reception_mode": "manual",
                              "call_type": "normal"}, sesid="mcv-smoke::5")
    mp5 = pl(r).get("member_ports", {})
    check("ADD g105 (call_type normal)", st(r)[0] == "OK" and len(mp5) == 3, f"{pl(r)}")
    r = req("PTT_GROUP_ADD", {"group_id": "g-bad", "members": f"{X}:5", "max_transmitters": 1, "call_type": "broadcast"},
            sesid="mcv-smoke::9")
    check("ADD call_type broadcast → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    sk, vr = {}, {}   # audio · video · control / 영상 RTCP (video + 1)
    for n in (X, Y, Z):
        v, vrt = udp_pair()
        sk[n], vr[n] = (udp(), v, udp()), vrt
    tcu = {X: 0x0C000001, Y: 0x0C000002, Z: 0x0C000003}
    tcs = {}
    for n, uri, pt, vpt in ((X, UX, 96, 98), (Y, UY, 97, 100), (Z, UZ, 96, 98)):
        a, v, c = sk[n]
        body = {"group_id": "g105", "session_id": n, "user_ip": IP, "user_port": a.getsockname()[1],
                "user_video_port": v.getsockname()[1], "user_control_port": c.getsockname()[1], "user_uri": uri,
                "user_tc_ssrc": tcu[n], "user_pt": pt, "user_video_pt": vpt}
        if n == X:
            body.update({"user_audio_ssrc": 0x5A000001, "user_video_ssrc": 0x5A000002})
        r = req("PTT_JOIN", body, sesid="mcv-smoke::5")
        tcs[n] = pl(r).get("tc_ssrc", 0)
    first = {n: msgs(sk[n][2]) for n in (X, Y, Z)}
    idle_ok = all(len(has(first[n], "MCV1", 0xF)) == 1 and has(first[n], "MCV1", 0xF)[0]["ssrc"] == tcu[n] for n in (X, Y, Z))
    check("join → Transmission Idle, header SSRC = user_tc_ssrc (§4.3.3.1)", idle_ok, f"{[(n, len(first[n])) for n in first]}")

    # 송출 요청 → Granted · Notification · TRANSMITTERS
    events.clear()
    sk[X][2].sendto(app(b"MCV0", 0, tcs[X]), (IP, mp5[X]["control_port"]))
    mx, my, mz = msgs(sk[X][2]), msgs(sk[Y][2]), msgs(sk[Z][2])
    g = has(mx, "MCV1", 0x0)
    ga, gv = (ssrc_f(g[0], 14), ssrc_f(g[0], 23)) if g else (0, 0)
    check("Transmission Granted — SSRC pair = offer a=ssrc (§14.3.7)", g and ga == 0x5A000001 and gv == 0x5A000002,
          f"{[hex(ga), hex(gv)]}")
    ny = has(my, "MCV1", 0x6)
    check("Media Transmission Notification to others — Transmitting User ID · manual",
          ny and ny[0]["f"].get(4, b"").decode() == UX and u16_f(ny[0], 22) == 1 and ssrc_f(ny[0], 23) == gv
          and has(mz, "MCV1", 0x6), f"{ny[:1]}")
    req("STATS", {}, service="system")
    tev = [e for e in events if e["hdr"]["cmd"] == "TRANSMITTERS"]
    check("TRANSMITTERS event [X]", tev and pl(tev[-1]).get("transmitters", [{}])[0].get("user") == X
          and pl(tev[-1])["transmitters"][0].get("video_ssrc") == gv and tev[-1]["hdr"].get("service") == "mcvideo",
          f"{[pl(e) for e in tev]}")

    # manual — [받기] 전에는 영상이 가지 않는다
    sk[X][1].sendto(rtp(pt=98, ssrc=0x77770001, seq=10), (IP, mp5[X]["video_port"]))
    check("manual — no video to Y before Receive Media Request", not drain(sk[Y][1], 0.3))
    # Receive Media Request → Response(granted, ack 비트) → Ack
    sk[Y][2].sendto(app(b"MCV0", 4, tcs[Y], tlv(4, UX.encode())), (IP, mp5[Y]["control_port"]))
    ry = has(msgs(sk[Y][2]), "MCV1", 0x7)
    check("Receive Media Response granted (ack bit — T6)", ry and u16_f(ry[0], 15) == 1 and ry[0]["ack"]
          and ssrc_f(ry[0], 23) == gv, f"{ry[:1]}")
    # 수신 시작 → 송출자에게 키프레임 요청 (B6 — RFC 4585 §6.3.1, 복합 = RR + SDES CNAME + PLI)
    kx = [parse_rtcp(d) for d in drain(vr[X], 0.3)]
    kp = psfb(kx[0], 1) if kx else []
    fb_ssrc = kx[0][0]["ssrc"] if kx else 0
    check("reception start → PLI to transmitter X (RR first · SDES CNAME · media source = X's original video SSRC)",
          len(kx) == 1 and kx[0][0]["pt"] == 201 and any(e["pt"] == 202 and e.get("cname") for e in kx[0])
          and kp and kp[0]["media"] == 0x77770001 and kp[0]["ssrc"] == fb_ssrc and fb_ssrc not in (0, gv),
          f"{kx}")
    sk[Y][2].sendto(app(b"MCV2", 4, tcs[Y], tlv(12, bytes([7, 0])) + tlv(10, struct.pack("!H", 0)) + tlv(16, b"MCV1\0\0")),
                    (IP, mp5[Y]["control_port"]))
    # 분배 — SSRC = 할당값, PT = 수신 leg 값
    sk[X][1].sendto(rtp(pt=98, ssrc=0x77770001, seq=11), (IP, mp5[X]["video_port"]))
    sk[X][0].sendto(rtp(pt=96, ssrc=0x77770002, seq=11), (IP, mp5[X]["port"]))
    vy, ay = drain(sk[Y][1], 0.3), drain(sk[Y][0], 0.3)
    check("video to Y — SSRC = granted video SSRC, PT = Y user_video_pt",
          vy and struct.unpack("!I", vy[0][8:12])[0] == gv and (vy[0][1] & 0x7F) == 100
          and struct.unpack("!H", vy[0][2:4])[0] == 11, f"{[d[:12].hex() for d in vy]}")
    check("audio to Y — SSRC = granted audio SSRC, PT = Y user_pt",
          ay and struct.unpack("!I", ay[0][8:12])[0] == ga and (ay[0][1] & 0x7F) == 97, f"{[d[:12].hex() for d in ay]}")
    check("Z (not receiving) gets no media", not drain(sk[Z][1], 0.2) and not drain(sk[Z][0], 0.1))

    # 수신자 피드백 → 송출자 (B6): SSRC 를 송출자 원래 값으로 되돌려 CMP 가 다시 보낸다, 송출자마다 500 ms 에 하나
    rr_y = struct.pack("!BBHI", 0x80, 201, 1, 0x0B0B0B0B)
    pli = lambda snd, media: struct.pack("!BBHII", 0x81, 206, 2, snd, media)
    fir = lambda snd, target, seq: struct.pack("!BBHIIIB3x", 0x84, 206, 4, snd, 0, target, seq)
    yrt, zrt = (IP, mp5[Y]["video_port"] + 1), (IP, mp5[Z]["video_port"] + 1)
    time.sleep(0.3)   # 수신 시작 PLI 로부터 간격 제한(500 ms)이 지나게
    vr[Y].sendto(rr_y + pli(0x0B0B0B0B, gv), yrt)
    vr[Y].sendto(rr_y + pli(0x0B0B0B0B, gv), yrt)   # 곧바로 두 번째 — 하나로 모인다
    f1 = [parse_rtcp(d) for d in drain(vr[X], 0.3)]
    p1 = psfb(f1[0], 1) if f1 else []
    check("receiver PLI (media = allocated SSRC) → one PLI to X, media = X's original SSRC (second throttled)",
          len(f1) == 1 and p1 and p1[0]["media"] == 0x77770001 and p1[0]["ssrc"] == fb_ssrc, f"{f1}")
    time.sleep(0.3)
    vr[Y].sendto(rr_y + fir(0x0B0B0B0B, gv, 7), yrt)
    f2 = [parse_rtcp(d) for d in drain(vr[X], 0.3)]
    p2 = psfb(f2[0], 4) if f2 else []
    fci = p2[0]["fci"] if p2 else b""
    check("receiver FIR → FIR to X (media source 0 · FCI SSRC = X's original · Seq nr = CMP's own 1)",
          p2 and p2[0]["media"] == 0 and len(fci) == 8 and struct.unpack("!I", fci[:4])[0] == 0x77770001 and fci[4] == 1,
          f"{f2}")
    time.sleep(0.6)
    vr[Z].sendto(struct.pack("!BBHI", 0x80, 201, 1, 0x0C0C0C0C) + pli(0x0C0C0C0C, gv), zrt)   # Z 는 받지 않는다
    vr[Y].sendto(pli(0x0B0B0B0B, 0x12345678), yrt)                                            # 없는 송출 (단독 PSFB)
    check("PLI from non-receiver Z / unknown SSRC not forwarded", not drain(vr[X], 0.4))
    check("STATS keyframe_requests 3 (reception start · PLI · FIR)",
          (mcv_group("g105") or {}).get("keyframe_requests") == 3, f"{mcv_group('g105')}")
    check("no Ack retransmission of the response after Ack", not has(msgs(sk[Y][2], 1.3), "MCV1", 0x7))

    # 헤더만 RTP = keepalive (판정 밖) · payload 있으면 Revoked #3
    sk[Z][0].sendto(struct.pack("!BBHII", 0x80, 96, 1, 1000, 0x7777000F), (IP, mp5[Z]["port"]))
    check("header-only RTP from Z ignored (keepalive)", not has(msgs(sk[Z][2], 0.3), "MCV1", 0x4))
    sk[Z][0].sendto(rtp(pt=96, ssrc=0x7777000F, seq=2), (IP, mp5[Z]["port"]))
    rz = has(msgs(sk[Z][2]), "MCV1", 0x4)
    check("media without permission → Revoked #3", rz and u16_f(rz[0], 2) == 3, f"{rz[:1]}")
    sk[Z][2].sendto(app(b"MCV2", 0, tcs[Z]), (IP, mp5[Z]["control_port"]))   # 그만 — End Response + Notification
    ez = msgs(sk[Z][2])
    check("Z End Request (not permitted) → End Response + Notification", has(ez, "MCV2", 0x1) and has(ez, "MCV1", 0x6))
    sk[Z][2].sendto(app(b"MCV0", 0, tcs[Z]), (IP, mp5[Z]["control_port"]))
    jz = has(msgs(sk[Z][2]), "MCV1", 0x1)
    check("limit (max_transmitters 1, no queueing) → Rejected #1", jz and u16_f(jz[0], 2) == 1, f"{jz[:1]}")
    g5 = mcv_group("g105") or {}
    check("STATS transmitters 1 · receptions 1", g5.get("transmitters") == 1 and g5.get("receptions") == 1, f"{g5}")

    # 송출 끝 → End Response · End Notify · Idle · TRANSMITTERS []
    events.clear()
    sk[X][2].sendto(app(b"MCV2", 0x10, tcs[X]), (IP, mp5[X]["control_port"]))   # ack 비트
    mx, my = msgs(sk[X][2]), msgs(sk[Y][2])
    ak = has(mx, "MCV2", 0x4)
    check("End Request ack bit → Transmission control Ack (Message Type 0 · Source 2 · MCV2)",
          ak and ak[0]["f"].get(12, b"\xff")[0] == 0 and u16_f(ak[0], 10) == 2 and ak[0]["f"].get(16, b"")[:4] == b"MCV2",
          f"{ak[:1]}")
    check("End Response to X", has(mx, "MCV2", 0x1))
    en = has(my, "MCV1", 0xE)
    check("End Notify + Idle to Y", en and ssrc_f(en[0], 23) == gv and has(my, "MCV1", 0xF))
    req("STATS", {}, service="system")
    tev = [e for e in events if e["hdr"]["cmd"] == "TRANSMITTERS"]
    check("TRANSMITTERS event []", tev and pl(tev[-1]).get("transmitters") == [], f"{[pl(e) for e in tev]}")
    sk[X][1].sendto(rtp(pt=98, ssrc=0x77770001, seq=12), (IP, mp5[X]["video_port"]))
    check("after end — X video not distributed", not drain(sk[Y][1], 0.3))

    # T1(Inactivity) — 짧은 hang timer 그룹
    r = req("PTT_GROUP_ADD", {"group_id": "g106", "members": f"{X}:5:participant", "max_transmitters": 1,
                              "tc_timers": {"t1_ms": 500, "t5_ms": 0}}, sesid="mcv-smoke::6")
    events.clear()
    q6 = udp()
    req("PTT_JOIN", {"group_id": "g106", "session_id": X, "user_ip": IP, "user_port": udp().getsockname()[1],
                     "user_control_port": q6.getsockname()[1]}, sesid="mcv-smoke::6")
    time.sleep(0.9)
    req("STATS", {}, service="system")
    iev = [e for e in events if e["hdr"]["cmd"] == "TRANSMISSION_INACTIVITY"]
    check("T1 expiry → TRANSMISSION_INACTIVITY {timer T1}", iev and pl(iev[0]).get("group_id") == "g106"
          and pl(iev[0]).get("timer") == "T1" and iev[0]["hdr"].get("sesid") == "mcv-smoke::6", f"{[pl(e) for e in iev]}")

    for gid, ses in (("g105", "mcv-smoke::5"), ("g106", "mcv-smoke::6")):
        req("PTT_GROUP_REMOVE", {"group_id": gid}, sesid=ses)
    res = pl(req("HEARTBEAT", {}, service="system"))["resource"]
    check("all released (after control flow)", res["mcvideo"]["member_used"] == 0 and res["mcvideo"]["groups"] == 0,
          f"{res['mcvideo']}")

    # ── 보호 (B7 — TS 33.180 제어 SRTCP · media_security.md SRTP) ──
    KG, SG = key_pair(0x11)                       # 그룹 tc_crypto
    KX, SX = key_pair(0x22)                       # X 의 CSK
    XVU, XVUS = key_pair(0x33)                    # X video 상향(UE→CMP)
    XVD, XVDS = key_pair(0x44)                    # X video 하향
    YVU, YVUS = key_pair(0x55)
    YVD, YVDS = key_pair(0x66)
    tcg = {"alg": "AES_CM_128_HMAC_SHA1_80", "key": b64(KG), "salt": b64(SG)}
    r = req("PTT_GROUP_ADD", {"group_id": "g107", "members": f"{X}:5,{Y}:3,{Z}:1", "max_transmitters": 1,
                              "tc_crypto": {"alg": "AES_CM_128_HMAC_SHA1_80", "key": b64(KG[:15]), "salt": b64(SG)}},
            sesid="mcv-smoke::7")
    check("ADD tc_crypto key 15 bytes → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g107", "members": f"{X}:5,{Y}:3,{Z}:1", "max_transmitters": 1,
                              "tc_crypto": tcg}, sesid="mcv-smoke::7")
    mp7 = pl(r).get("member_ports", {})
    check("ADD g107 with tc_crypto (group key)", st(r)[0] == "OK" and len(mp7) == 3, f"{pl(r)}")
    r = req("PTT_JOIN", {"group_id": "g107", "session_id": Z, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "floor_crypto": tcg}, sesid="mcv-smoke::7")
    check("JOIN floor_crypto → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    sk7, vr7 = {}, {}
    for n in (X, Y, Z):
        v, vrt = udp_pair()
        sk7[n], vr7[n] = (udp(), v, udp()), vrt
    ukey = {X: 0x0D000001, Y: 0x0D000002, Z: 0x0D000003}
    tc7 = {}

    def mc(rx, rxs, tx, txs):
        return {"alg": "AES_CM_128_HMAC_SHA1_80", "rx": {"key": b64(rx), "salt": b64(rxs)},
                "tx": {"key": b64(tx), "salt": b64(txs)}}

    for n, uri, extra in ((X, UX, {"tc_crypto": {"alg": "AES_CM_128_HMAC_SHA1_80", "key": b64(KX), "salt": b64(SX)},
                                   "media_crypto_video": mc(XVU, XVUS, XVD, XVDS)}),
                          (Y, UY, {"media_crypto_video": mc(YVU, YVUS, YVD, YVDS), "user_video_pt": 100}),
                          (Z, UZ, {})):
        a, v, c = sk7[n]
        body = {"group_id": "g107", "session_id": n, "user_ip": IP, "user_port": a.getsockname()[1],
                "user_video_port": v.getsockname()[1], "user_control_port": c.getsockname()[1], "user_uri": uri,
                "user_tc_ssrc": ukey[n]}
        body.update(extra)
        r = req("PTT_JOIN", body, sesid="mcv-smoke::7")
        tc7[n] = pl(r).get("tc_ssrc", 0)
    cx, cg = Srtp(KX, SX), Srtp(KG, SG)          # 단말 쪽 SRTCP 컨텍스트 — X = CSK, Y·Z = 그룹 키
    cy, cz = Srtp(KG, SG), Srtp(KG, SG)
    dx = drain(sk7[X][2])
    ix = [parse_app(cx.unprotect_rtcp(d) or b"") for d in dx]
    iy, iz = smsgs(sk7[Y][2], cy), smsgs(sk7[Z][2], cz)
    check("Idle to X protected with X's CSK", ix and ix[0] and ix[0]["name"] == "MCV1" and ix[0]["subtype"] == 0xF
          and ix[0]["ssrc"] == ukey[X], f"{ix}")
    check("Idle to Y·Z protected with group key", iy and iy[0] and iy[0]["subtype"] == 0xF and iz and iz[0]
          and iz[0]["subtype"] == 0xF, f"{iy} {iz}")
    check("X's Idle is not readable with the group key (member CSK wins)", dx and cg.unprotect_rtcp(dx[0]) is None
          and dx[0][8:12] != b"MCV1", f"{[d[:16].hex() for d in dx]}")   # name 은 암호화 범위(헤더 8B 뒤)
    before = (mcv_group("g107") or {}).get("crypto_drop", 0)
    sk7[Z][2].sendto(app(b"MCV0", 0, tc7[Z]), (IP, mp7[Z]["control_port"]))            # 평문 — 버림
    sk7[Z][2].sendto(Srtp(KX, SX).protect_rtcp(app(b"MCV0", 0, tc7[Z])), (IP, mp7[Z]["control_port"]))  # 남의 키 — 버림
    zr = drain(sk7[Z][2], 0.3)
    check("plain / wrong-key SRTCP request dropped (no answer)", not zr, f"{len(zr)} packets")
    check("STATS crypto_drop counts them", (mcv_group("g107") or {}).get("crypto_drop", 0) - before == 2)
    sk7[X][2].sendto(cx.protect_rtcp(app(b"MCV0", 0, tc7[X])), (IP, mp7[X]["control_port"]))
    gx = [m for m in smsgs(sk7[X][2], cx) if m]
    g7 = has(gx, "MCV1", 0x0)
    gv7 = ssrc_f(g7[0], 23) if g7 else 0
    check("SRTCP Transmission Request → Granted (X's CSK both ways)", g7 and gv7 > 0, f"{gx}")
    ny7 = [m for m in smsgs(sk7[Y][2], cy) if m]
    check("Notification to Y under group key", has(ny7, "MCV1", 0x6), f"{ny7}")
    sk7[Y][2].sendto(cy.protect_rtcp(app(b"MCV0", 4, tc7[Y], tlv(4, UX.encode()))), (IP, mp7[Y]["control_port"]))
    ry7 = has([m for m in smsgs(sk7[Y][2], cy) if m], "MCV1", 0x7)
    check("Receive Media Request/Response under group key", ry7 and u16_f(ry7[0], 15) == 1, f"{ry7}")
    # 영상 SRTP — X 상향 키로 보호해 보내면 CMP 가 풀고, SSRC·PT 를 찍어 Y 하향 키로 다시 보호한다
    ux, dy = Srtp(XVU, XVUS), Srtp(YVD, YVDS)
    sk7[X][1].sendto(ux.protect_rtp(rtp(pt=98, ssrc=0x78780001, seq=200)), (IP, mp7[X]["video_port"]))
    vy7 = drain(sk7[Y][1], 0.3)
    plain = dy.unprotect_rtp(vy7[0]) if vy7 else None
    check("video SRTP: X uplink key → Y downlink key (SSRC·PT stamped under protection)",
          plain is not None and struct.unpack("!I", plain[8:12])[0] == gv7 and (plain[1] & 0x7F) == 100
          and plain[12:] == b"\0" * 20, f"{[d[:12].hex() for d in vy7]}")
    check("Y downlink not readable with X's uplink key", vy7 and Srtp(XVU, XVUS).unprotect_rtp(vy7[0]) is None)
    before = (mcv_group("g107") or {}).get("crypto_drop", 0)
    sk7[X][1].sendto(Srtp(YVU, YVUS).protect_rtp(rtp(pt=98, ssrc=0x78780001, seq=201)), (IP, mp7[X]["video_port"]))
    check("wrong-key video dropped", not drain(sk7[Y][1], 0.3)
          and (mcv_group("g107") or {}).get("crypto_drop", 0) - before == 1)
    # 영상 SRTCP 키프레임 요청 (B6) — Y 상향 영상 키로 보호한 PLI 를 CMP 가 풀고, X 하향 영상 키로 다시 보호해 X 에게
    drain(vr7[X], 0.1)
    fb7 = struct.pack("!BBHI", 0x80, 201, 1, 0x0B0B0B0C) + struct.pack("!BBHII", 0x81, 206, 2, 0x0B0B0B0C, gv7)
    vr7[Y].sendto(Srtp(YVU, YVUS).protect_rtcp(fb7), (IP, mp7[Y]["video_port"] + 1))
    kx7 = drain(vr7[X], 0.3)
    pk7 = parse_rtcp(Srtp(XVD, XVDS).unprotect_rtcp(kx7[0]) or b"") if kx7 else []
    check("video SRTCP PLI: Y uplink key → X downlink key, media = X's original SSRC",
          psfb(pk7, 1) and psfb(pk7, 1)[0]["media"] == 0x78780001, f"{pk7} {[d[:8].hex() for d in kx7]}")
    before = (mcv_group("g107") or {}).get("crypto_drop", 0)
    vr7[Y].sendto(fb7, (IP, mp7[Y]["video_port"] + 1))   # 평문 — SRTCP leg 라 인증 실패로 버린다
    check("plaintext RTCP on SRTCP leg dropped (crypto_drop +1)", not drain(vr7[X], 0.3)
          and (mcv_group("g107") or {}).get("crypto_drop", 0) - before == 1)
    r = req("PTT_JOIN", {"group_id": "g107", "session_id": X, "user_ip": IP, "user_port": sk7[X][0].getsockname()[1],
                         "user_video_port": sk7[X][1].getsockname()[1], "user_control_port": sk7[X][2].getsockname()[1],
                         "user_uri": UX, "user_tc_ssrc": ukey[X],
                         "tc_crypto": {"alg": "AES_CM_128_HMAC_SHA1_80", "key": b64(KX), "salt": b64(SX)},
                         "media_crypto_video": mc(XVU, XVUS, XVD, XVDS)}, sesid="mcv-smoke::7")
    sk7[X][2].sendto(cx.protect_rtcp(app(b"MCV0", 0, tc7[X])), (IP, mp7[X]["control_port"]))
    rg = [m for m in smsgs(sk7[X][2], cx) if m]
    check("JOIN ② resend with the same keys keeps SRTCP context (Granted again, index continues)",
          st(r)[0] == "OK" and has(rg, "MCV1", 0x0), f"{rg}")
    req("PTT_GROUP_REMOVE", {"group_id": "g107"}, sesid="mcv-smoke::7")
    res = pl(req("HEARTBEAT", {}, service="system"))["resource"]
    check("all released (after protection)", res["mcvideo"]["member_used"] == 0 and res["mcvideo"]["groups"] == 0,
          f"{res['mcvideo']}")
finally:
    if proc:
        alive = proc.poll() is None
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if not alive:
            print("FAIL  CMP exited during test (see log)")
            results.append(False)
        if all(results) and tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        elif tmp:
            print(f"CMP log kept: {tmp}")

n_ok = sum(results)
print(f"\n{n_ok}/{len(results)} PASS")
sys.exit(0 if results and all(results) else 1)
