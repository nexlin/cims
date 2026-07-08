#!/usr/bin/env python3
"""
MCData SDS over media plane (MSRP, TS 24.282 §9.2.3) E2E 테스트 클라이언트.

라이브 CSP+cmdp 를 상대로 표준 단말 역할을 흉내낸다 (raw socket, 의존성 없음).

모드:
  sender    REGISTER → INVITE(더미 m=audio + m=message TCP/MSRP sendonly)
            → 200 의 a=path 로 TCP 접속 → SDS TLV 2건 SEND → 200/REPORT → 서버 BYE 수신
  receiver  MCData ICSI feature tag 로 REGISTER → 서버발 INVITE 대기 → 200 answer
            (audio inactive + m=message recvonly, a=setup:active) → 서버 path 접속
            → SEND 수신·200 → BYE 수신. 수신 본문 텍스트 출력.
  fallback  태그 없이 REGISTER → FD SIGNALLING(FILEURL) MESSAGE 수신 대기 → 200 → URL 출력.
  negative  C-plane 임계 초과 MESSAGE 송신 → 403 + Warning 203 확인.

사용 예 (ctrl02 등 시험 머신에서):
  python3 tests/msrp_sds_client.py sender   --ip 192.168.0.47 --user 1001 --group 9001 --text "$(python3 -c 'print("x"*3000)')"
  python3 tests/msrp_sds_client.py receiver --ip 192.168.0.47 --user 1002
  python3 tests/msrp_sds_client.py fallback --ip 192.168.0.47 --user 1003
  python3 tests/msrp_sds_client.py negative --ip 192.168.0.47 --user 1001 --group 9001
"""
import argparse
import hashlib
import re
import socket
import struct
import sys
import time
import uuid

GRN, RED, CYN, NC = "\033[32m", "\033[31m", "\033[36m", "\033[0m"
def ok(m):   print(f"  {GRN}[PASS]{NC} {m}")
def fail(m): print(f"  {RED}[FAIL]{NC} {m}")
def info(m): print(f"  {CYN}[INFO]{NC} {m}")

MCDATA_ICSI = 'urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds'
ACCEPT_TYPES = ("multipart/mixed application/vnd.3gpp.mcdata-signalling "
                "application/vnd.3gpp.mcdata-payload")


def _md5(s): return hashlib.md5(s.encode()).hexdigest()

def digest(username, realm, password, method, uri, nonce, qop="auth", cnonce="1", nc="00000001"):
    ha1 = _md5(f"{username}:{realm}:{password}")
    ha2 = _md5(f"{method}:{uri}")
    if qop:
        return _md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
    return _md5(f"{ha1}:{nonce}:{ha2}")


class SipUa:
    """최소 SIP UA (UDP) — REGISTER digest / INVITE(UAC·UAS) / MESSAGE."""

    def __init__(self, server_ip, server_port, user, domain, password, feature_tag="",
                 auth_user=""):
        self.server = (server_ip, server_port)
        self.user, self.domain, self.password = user, domain, password
        self.auth_user = auth_user or user  # digest username (CIMS v3: imsi@svc-domain)
        self.feature_tag = feature_tag
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect(self.server)          # 로컬 IP 확정용
        self.local_ip = self.sock.getsockname()[0]
        self.sock.close()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((self.local_ip, 0))
        self.local_port = self.sock.getsockname()[1]

    # ── 저수준 ──
    def send(self, data):
        self.sock.sendto(data if isinstance(data, bytes) else data.encode(), self.server)

    def recv(self, timeout=5.0):
        self.sock.settimeout(timeout)
        try:
            data, _ = self.sock.recvfrom(65535)
            return data.decode(errors="replace")
        except socket.timeout:
            return ""

    def wait_for(self, pred, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            msg = self.recv(min(2.0, end - time.time()))
            if msg and pred(msg):
                return msg
        return ""

    @staticmethod
    def header(msg, name):
        m = re.search(rf"^{name}\s*:\s*(.+?)\r?$", msg, re.M | re.I)
        return m.group(1).strip() if m else ""

    def _via(self):
        return f"SIP/2.0/UDP {self.local_ip}:{self.local_port};branch=z9hG4bK{uuid.uuid4().hex[:10]};rport"

    def contact(self):
        c = f"<sip:{self.user}@{self.local_ip}:{self.local_port}>"
        if self.feature_tag:
            c += f';+g.3gpp.icsi-ref="{self.feature_tag}"'
        return c

    # ── REGISTER (401 digest 재시도) ──
    def register(self):
        call_id = uuid.uuid4().hex
        tag = uuid.uuid4().hex[:8]
        uri = f"sip:{self.domain}"

        def build(cseq, auth=""):
            lines = [
                f"REGISTER {uri} SIP/2.0",
                f"Via: {self._via()}",
                f"From: <sip:{self.user}@{self.domain}>;tag={tag}",
                f"To: <sip:{self.user}@{self.domain}>",
                f"Call-ID: {call_id}",
                f"CSeq: {cseq} REGISTER",
                f"Contact: {self.contact()}",
                "Max-Forwards: 70", "Expires: 600",
                "User-Agent: MSRP-SDS-Test/1.0",
            ]
            if auth:
                lines.append(auth)
            lines += ["Content-Length: 0", "", ""]
            return "\r\n".join(lines)

        self.send(build(1))
        rsp = self.wait_for(lambda m: m.startswith("SIP/2.0") and " REGISTER" in self.header(m, "CSeq"))
        if rsp.startswith("SIP/2.0 200"):
            return True
        m = re.search(r'nonce="([^"]+)"', rsp)
        realm = (re.search(r'realm="([^"]+)"', rsp) or [None, self.domain])[1]
        if not m:
            fail(f"REGISTER 응답에 nonce 없음: {rsp.splitlines()[0] if rsp else 'timeout'}")
            return False
        resp = digest(self.auth_user, realm, self.password, "REGISTER", uri, m.group(1))
        auth = (f'Authorization: Digest username="{self.auth_user}", realm="{realm}", nonce="{m.group(1)}", '
                f'uri="{uri}", response="{resp}", algorithm=MD5, cnonce="1", qop=auth, nc=00000001')
        self.send(build(2, auth))
        rsp = self.wait_for(lambda m2: m2.startswith("SIP/2.0") and " REGISTER" in self.header(m2, "CSeq"))
        return rsp.startswith("SIP/2.0 200")

    # ── MESSAGE 송신 → 최종응답 반환 ──
    def message(self, to, content_type, body):
        call_id = uuid.uuid4().hex
        req = "\r\n".join([
            f"MESSAGE sip:{to}@{self.domain} SIP/2.0",
            f"Via: {self._via()}",
            f"From: <sip:{self.user}@{self.domain}>;tag={uuid.uuid4().hex[:8]}",
            f"To: <sip:{to}@{self.domain}>",
            f"Call-ID: {call_id}",
            "CSeq: 1 MESSAGE",
            "Max-Forwards: 70",
            f"Content-Type: {content_type}",
            f"Content-Length: {len(body.encode())}", "", body])
        self.send(req)
        return self.wait_for(lambda m: m.startswith("SIP/2.0") and call_id in m) or ""

    # ── UAC INVITE — (최종응답, dialog dict) ──
    def invite(self, to, sdp, extra_headers=()):
        call_id = uuid.uuid4().hex
        tag = uuid.uuid4().hex[:8]
        lines = [
            f"INVITE sip:{to}@{self.domain} SIP/2.0",
            f"Via: {self._via()}",
            f"From: <sip:{self.user}@{self.domain}>;tag={tag}",
            f"To: <sip:{to}@{self.domain}>",
            f"Call-ID: {call_id}",
            "CSeq: 1 INVITE",
            f"Contact: {self.contact()}",
            "Max-Forwards: 70",
        ] + list(extra_headers) + [
            "Content-Type: application/sdp",
            f"Content-Length: {len(sdp.encode())}", "", sdp]
        self.send("\r\n".join(lines))

        final = ""
        end = time.time() + 15
        while time.time() < end:
            msg = self.recv(2.0)
            if not msg or call_id not in msg:
                continue
            code = int(msg.split()[1]) if msg.startswith("SIP/2.0") else 0
            if code >= 200:
                final = msg
                break
        if not final:
            return "", {}
        to_hdr = self.header(final, "To")
        ack = "\r\n".join([
            f"ACK sip:{to}@{self.domain} SIP/2.0",
            f"Via: {self._via()}",
            f"From: <sip:{self.user}@{self.domain}>;tag={tag}",
            f"To: {to_hdr}",
            f"Call-ID: {call_id}",
            "CSeq: 1 ACK", "Max-Forwards: 70", "Content-Length: 0", "", ""])
        self.send(ack)
        return final, {"call_id": call_id, "tag": tag, "to": to_hdr}

    def bye(self, dlg, to):
        self.send("\r\n".join([
            f"BYE sip:{to}@{self.domain} SIP/2.0",
            f"Via: {self._via()}",
            f"From: <sip:{self.user}@{self.domain}>;tag={dlg['tag']}",
            f"To: {dlg['to']}",
            f"Call-ID: {dlg['call_id']}",
            "CSeq: 2 BYE", "Max-Forwards: 70", "Content-Length: 0", "", ""]))

    def respond(self, req, code, reason, extra_headers=(), body="", content_type=""):
        via = self.header(req, "Via")
        frm = self.header(req, "From")
        to = self.header(req, "To")
        if code != 100 and "tag=" not in to:
            to += f";tag={uuid.uuid4().hex[:8]}"
        lines = [f"SIP/2.0 {code} {reason}", f"Via: {via}", f"From: {frm}", f"To: {to}",
                 f"Call-ID: {self.header(req, 'Call-ID')}", f"CSeq: {self.header(req, 'CSeq')}"]
        lines += list(extra_headers)
        if body:
            lines += [f"Content-Type: {content_type}", f"Content-Length: {len(body.encode())}", "", body]
        else:
            lines += ["Content-Length: 0", "", ""]
        self.send("\r\n".join(lines))
        return to


# ── MCData TLV ──────────────────────────────────────────────────────────────

def tlv_signalling(conv_hex, msg_hex, disposition=True):
    b = bytes([0x01]) + int(time.time()).to_bytes(5, "big")
    b += bytes.fromhex(conv_hex) + bytes.fromhex(msg_hex)
    if disposition:
        b += bytes([0x81])
    return b

def tlv_payload(text):
    tb = text.encode()
    return bytes([0x03, 1, 0x78]) + struct.pack(">H", 1 + len(tb)) + bytes([0x01]) + tb


# ── MSRP ────────────────────────────────────────────────────────────────────

def msrp_send_frame(tid, to_path, from_path, msg_id, ct, body, success_report=False, flag="$"):
    h = (f"MSRP {tid} SEND\r\nTo-Path: {to_path}\r\nFrom-Path: {from_path}\r\n"
         f"Message-ID: {msg_id}\r\nByte-Range: 1-{len(body)}/{len(body)}\r\n")
    if success_report:
        h += "Success-Report: yes\r\n"
    h += "Failure-Report: yes\r\n"
    if ct:
        h += f"Content-Type: {ct}\r\n\r\n"
        return h.encode() + body + f"\r\n-------{tid}{flag}\r\n".encode()
    return h.encode() + f"-------{tid}{flag}\r\n".encode()


def msrp_read_frames(sock, want, timeout=8.0, buf=None):
    """buf: bytearray — 호출 간 잔여 스트림 보존용(한 recv 에 여러 프레임 도착 대응)."""
    sock.settimeout(timeout)
    if buf is None:
        buf = bytearray()
    out = []
    # 이전 호출 잔여분에서 먼저 프레임 추출
    _extract_frames(buf, out, want)
    while len(out) < want:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf.extend(chunk)
        _extract_frames(buf, out, want)
    return out


def _extract_frames(buf, out, want):
    """bytearray buf 에서 완성 프레임을 in-place 로 잘라 out 에 적재."""
    while len(out) < want:
        m = buf.find(b"MSRP ")
        if m < 0:
            return
        eol = buf.find(b"\r\n", m)
        if eol < 0:
            return
        tid = bytes(buf[m:eol]).split()[1]
        e = buf.find(b"-------" + tid, eol)
        if e < 0 or e + 7 + len(tid) + 3 > len(buf):
            return
        out.append(bytes(buf[m:e + 7 + len(tid) + 3]))
        del buf[:e + 7 + len(tid) + 3]


def sdp_path_of(sdp_or_sip):
    m = re.search(r"a=path:(\S+)", sdp_or_sip)
    return m.group(1) if m else ""


def msrp_offer_sdp(local_ip, session):
    """더미 m=audio + m=message sendonly 오퍼 (서버·앱 계약 SDP 프로파일)."""
    return "\r\n".join([
        "v=0", f"o=- 1 1 IN IP4 {local_ip}", "s=-", f"c=IN IP4 {local_ip}", "t=0 0",
        "m=audio 4000 RTP/AVP 0", "a=rtpmap:0 PCMU/8000", "a=sendrecv",
        "m=message 2855 TCP/MSRP *",
        f"a=path:msrp://{local_ip}:2855/{session};tcp",
        f"a=accept-types:{ACCEPT_TYPES}",
        "a=setup:actpass", "a=sendonly", ""])


# ── 모드 구현 ────────────────────────────────────────────────────────────────

def mode_sender(args):
    ua = SipUa(args.ip, args.port, args.user, args.domain, args.password, auth_user=args.auth_user)
    if not ua.register():
        fail("REGISTER 실패")
        return 1
    ok(f"REGISTER {args.user}")

    session = uuid.uuid4().hex[:12]
    final, dlg = ua.invite(args.group, msrp_offer_sdp(ua.local_ip, session),
                           [f'Accept-Contact: *;+g.3gpp.icsi-ref="{MCDATA_ICSI}";require;explicit',
                            "P-Preferred-Service: urn:urn-7:3gpp-service.ims.icsi.mcdata.sds"])
    if not final.startswith("SIP/2.0 200"):
        fail(f"INVITE 최종응답: {final.splitlines()[0] if final else 'timeout'}")
        return 1
    server_path = sdp_path_of(final)
    audio_m = re.search(r"m=audio (\d+)", final)
    if not (audio_m and audio_m.group(1) != "0" and "a=inactive" in final):
        fail(f"오디오 라인 계약 위반 (포트≠0 + inactive 기대): m=audio {audio_m.group(1) if audio_m else '?'}")
    else:
        ok(f"200 OK — audio port={audio_m.group(1)} inactive, path={server_path}")
    if not server_path:
        fail("200 OK 에 a=path 없음")
        return 1

    host, port = re.match(r"msrp://([^:/]+):(\d+)/", server_path).groups()
    s = socket.create_connection((host, int(port)), timeout=5)
    rxbuf = bytearray()  # 호출 간 스트림 잔여분 보존
    local_path = f"msrp://{ua.local_ip}:2855/{session};tcp"
    conv, msgid = uuid.uuid4().hex, uuid.uuid4().hex
    s.sendall(msrp_send_frame("t1" + uuid.uuid4().hex[:6], server_path, local_path, "m1",
                              "application/vnd.3gpp.mcdata-signalling", tlv_signalling(conv, msgid)))
    f1 = msrp_read_frames(s, 1, buf=rxbuf)
    ok("signalling SEND → 200") if f1 and b" 200" in f1[0].splitlines()[0] else fail("signalling 200 미수신")
    s.sendall(msrp_send_frame("t2" + uuid.uuid4().hex[:6], server_path, local_path, "m2",
                              "application/vnd.3gpp.mcdata-payload", tlv_payload(args.text), True))
    f2 = msrp_read_frames(s, 2, buf=rxbuf)
    ok("payload SEND → 200+REPORT") if len(f2) >= 2 else fail(f"payload 응답 부족: {len(f2)}")

    bye = ua.wait_for(lambda m: m.startswith("BYE "), timeout=10)
    if bye:
        ua.respond(bye, 200, "OK")
        ok("서버 BYE 수신 → 전송 완료")
    else:
        fail("서버 BYE 미수신")
    info(f"conv={conv} msg={msgid} bytes={len(args.text.encode())}")
    return 0


def mode_receiver(args):
    ua = SipUa(args.ip, args.port, args.user, args.domain, args.password, feature_tag=MCDATA_ICSI, auth_user=args.auth_user)
    if not ua.register():
        fail("REGISTER 실패")
        return 1
    ok(f"REGISTER {args.user} (+g.3gpp.icsi-ref mcdata — MSRP 배포 대상)")
    info("서버발 INVITE 대기 중... (다른 창에서 sender 실행)")

    inv = ua.wait_for(lambda m: m.startswith("INVITE "), timeout=args.wait)
    if not inv:
        fail("INVITE 미수신")
        return 1
    server_path = sdp_path_of(inv)
    ok(f"INVITE 수신 — server path={server_path}")

    session = uuid.uuid4().hex[:12]
    answer = "\r\n".join([
        "v=0", f"o=- 1 1 IN IP4 {ua.local_ip}", "s=-", f"c=IN IP4 {ua.local_ip}", "t=0 0",
        "m=audio 9 RTP/AVP 0", "a=inactive",
        "m=message 2855 TCP/MSRP *",
        f"a=path:msrp://{ua.local_ip}:2855/{session};tcp",
        f"a=accept-types:{ACCEPT_TYPES}",
        "a=setup:active", "a=recvonly", ""])
    ua.respond(inv, 200, "OK", [f"Contact: {ua.contact()}"], answer, "application/sdp")

    host, port = re.match(r"msrp://([^:/]+):(\d+)/", server_path).groups()
    s = socket.create_connection((host, int(port)), timeout=5)
    rxbuf = bytearray()  # 호출 간 스트림 잔여분 보존
    local_path = f"msrp://{ua.local_ip}:2855/{session};tcp"
    tid = "bnd" + uuid.uuid4().hex[:6]
    s.sendall(msrp_send_frame(tid, server_path, local_path, "b0", "", b""))  # bodiless 바인딩

    body = b""
    done = False
    while not done:
        frames = msrp_read_frames(s, 1, timeout=10, buf=rxbuf)
        if not frames:
            break
        for f in frames:
            start = f.splitlines()[0]
            if b" SEND" not in start:
                continue
            ftid = start.split()[1]
            blank = f.find(b"\r\n\r\n")
            endm = f.rfind(b"\r\n-------")
            if blank > 0 and endm > blank:
                body += f[blank + 4:endm]
            s.sendall(f"MSRP {ftid.decode()} 200 OK\r\nTo-Path: {server_path}\r\n"
                      f"From-Path: {local_path}\r\n-------{ftid.decode()}$\r\n".encode())
            if f.rstrip().endswith(ftid + b"$") or (b"-------" + ftid + b"$") in f:
                done = True
    ok(f"MSRP 수신 완료 — {len(body)} bytes")
    m = re.search(rb"\x78(..)\x01", body, re.S)
    if b"mcdata-payload" in body:
        ok("multipart 본문에 mcdata-payload 포함")
    print("---- 수신 본문 (앞 400B) ----")
    print(body[:400].decode(errors="replace"))

    bye = ua.wait_for(lambda m2: m2.startswith("BYE "), timeout=10)
    if bye:
        ua.respond(bye, 200, "OK")
        ok("서버 BYE 수신")
    return 0


def mode_fallback(args):
    ua = SipUa(args.ip, args.port, args.user, args.domain, args.password, auth_user=args.auth_user)  # 태그 없음
    if not ua.register():
        fail("REGISTER 실패")
        return 1
    ok(f"REGISTER {args.user} (태그 없음 — FILEURL 폴백 대상)")
    info("FD SIGNALLING MESSAGE 대기 중... (다른 창에서 sender 실행)")
    msg = ua.wait_for(lambda m: m.startswith("MESSAGE ") and "mcdata" in m, timeout=args.wait)
    if not msg:
        fail("MESSAGE 미수신")
        return 1
    ua.respond(msg, 200, "OK")
    ok("FD SIGNALLING MESSAGE 수신")
    import base64
    b64 = re.search(r"base64\r\n\r\n([A-Za-z0-9+/=\r\n]+?)\r\n--", msg)
    if b64:
        raw = base64.b64decode(b64.group(1).replace("\r\n", ""))
        um = re.search(rb"\x04(https?://[^\x79]+)", raw, re.S)
        if um:
            ok(f"FILEURL: {um.group(1).decode(errors='replace')}")
    return 0


def mode_negative(args):
    ua = SipUa(args.ip, args.port, args.user, args.domain, args.password, auth_user=args.auth_user)
    if not ua.register():
        fail("REGISTER 실패")
        return 1
    # C-plane 임계 초과 payload — text/plain 그룹 문자 (payload=본문 길이)
    big = "y" * args.oversize
    rsp = ua.message(args.group, "text/plain", big)
    first = rsp.splitlines()[0] if rsp else "timeout"
    warning = SipUa.header(rsp, "Warning") if rsp else ""
    if first.startswith("SIP/2.0 403") and warning.startswith("203"):
        ok(f"403 + Warning: {warning}")
        return 0
    fail(f"기대 403+Warning 203, 실제: {first} / Warning: {warning}"
         " (Setup.McData.MaxPayloadSizeSdsCplaneBytes 설정 확인)")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["sender", "receiver", "fallback", "negative"])
    ap.add_argument("--ip", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15060)
    ap.add_argument("--user", default="1001")
    ap.add_argument("--group", default="9001")
    ap.add_argument("--domain", default="ptt.mnc033.mcc450.3gppnetwork.org")
    ap.add_argument("--password", default="1234")
    ap.add_argument("--auth-user", default="", help="digest username (기본: user; CIMS v3 는 imsi@svc-domain)")
    ap.add_argument("--text", default="MSRP 대용량 SDS 시험 " + "x" * 2500)
    ap.add_argument("--oversize", type=int, default=20000, help="negative 모드 payload 크기")
    ap.add_argument("--wait", type=int, default=120, help="receiver/fallback 대기 초")
    args = ap.parse_args()
    return {"sender": mode_sender, "receiver": mode_receiver,
            "fallback": mode_fallback, "negative": mode_negative}[args.mode](args)


if __name__ == "__main__":
    sys.exit(main())
