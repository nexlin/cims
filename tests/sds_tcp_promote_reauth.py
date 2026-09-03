#!/usr/bin/env python3
"""SDS MESSAGE 의 TCP 승격 + Digest 재인증 서버 계약 검증 (mcdata_messaging.md §4, registration_binding_set.md §3).

UDP 등록 가입자의 MESSAGE 가 별도 TCP 연결(등록 flow 밖)로 도착하면 CSP 는 401 로 재인증하고,
Authorization 재발행(CSeq+1)을 받아 200 + 상대(UDP 등록)에 전달하며, 발신자의 UDP 바인딩은 만들거나
옮기지 않는다 — 단말(pjsip)이 1300B 초과 요청을 RFC 3261 §18.1.1 로 TCP 승격할 때의 서버 측 전제.

사용법 (CSP 가 도는 개발 서버에서, 시험 계정 010+ 대역):
  python3 tests/sds_tcp_promote_reauth.py                 # 011→012, 비밀번호 123456
  python3 tests/sds_tcp_promote_reauth.py --a 13 --b 14 --password 123456 --ip 121.161.164.45

  A(011) UDP REGISTER → 200
  B(012) UDP REGISTER → 200
  A ── TCP MESSAGE(one-to-one-sds, ≈1.6KB) ──→ 401 → Authorization 재발행(CSeq+1) → 200
  B UDP 수신 MESSAGE 본문 검증 → 200 응답
  B ── UDP MESSAGE(소형) ──→ 200, A UDP 수신 → 200  (A 바인딩 무손상 확인)
  A/B REGISTER Expires:0
"""
import argparse, base64, hashlib, re, socket, struct, sys, time, uuid

SERVER = "121.161.164.45"; PORT = 15060
DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"
LOCAL_IP = "121.161.164.45"   # Via/Contact 광고 주소 (--ip 로 서버·로컬 동시 지정)

def md5(s): return hashlib.md5(s.encode()).hexdigest()
def digest(user, realm, pw, method, uri, nonce, qop, cnonce="abc", nc="00000001"):
    ha1 = md5(f"{user}:{realm}:{pw}"); ha2 = md5(f"{method}:{uri}")
    return md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}") if qop else md5(f"{ha1}:{nonce}:{ha2}")
def auth_hdr(user, realm, pw, method, uri, nonce, qop):
    r = digest(user, realm, pw, method, uri, nonce, qop)
    h = f'Authorization: Digest username="{user}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{r}", algorithm=MD5'
    if qop: h += f', cnonce="abc", qop={qop}, nc=00000001'
    return h
def parse_chal(msg):
    m = re.search(r"WWW-Authenticate:\s*Digest\s+(.*)", msg, re.I)
    d = dict(re.findall(r'(\w+)="?([^",]+)"?', m.group(1)))
    return d["realm"], d["nonce"], d.get("qop")
def status(msg): return int(msg.split(" ", 2)[1])
def hdr(msg, name):
    m = re.search(rf"^{name}:\s*(.*)$", msg, re.I | re.M); return m.group(1).strip() if m else ""

def recv_sip(sock_recv):
    """TCP: 헤더+Content-Length 만큼 읽기."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = sock_recv(4096)
        if not d: raise RuntimeError("closed")
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    cl = int(hdr(head.decode(errors="replace"), "Content-Length") or 0)
    while len(rest) < cl:
        d = sock_recv(4096)
        if not d: break
        rest += d
    return (head + b"\r\n\r\n" + rest[:cl]).decode(errors="replace")

class Ua:
    def __init__(self, num, imsi, pw, port):
        self.num, self.impi, self.pw, self.port = num, f"{imsi}@{DOMAIN}", pw, port
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind((LOCAL_IP, port)); self.udp.settimeout(6)
        self.callid = str(uuid.uuid4()); self.cseq = 1; self.tag = uuid.uuid4().hex[:8]
    def aor(self): return f"sip:{self.num}@{DOMAIN}"
    def register(self, expires=300):
        uri = f"sip:{DOMAIN}"; auth = None; code = 0; rsp = ""
        for _ in range(3):
            self.cseq += 1
            h = [f"REGISTER {uri} SIP/2.0",
                 f"Via: SIP/2.0/UDP {LOCAL_IP}:{self.port};rport;branch=z9hG4bK{uuid.uuid4().hex[:12]}",
                 "Max-Forwards: 70", f"From: <{self.aor()}>;tag={self.tag}", f"To: <{self.aor()}>",
                 f"Call-ID: {self.callid}", f"CSeq: {self.cseq} REGISTER",
                 f"Contact: <sip:{self.num}@{LOCAL_IP}:{self.port}>", f"Expires: {expires}",
                 "User-Agent: cims-premise-test"]
            if auth: h.append(auth)
            self.udp.sendto(("\r\n".join(h) + "\r\nContent-Length: 0\r\n\r\n").encode(), (SERVER, PORT))
            rsp = self.recv_udp()[0]; code = status(rsp)
            if code == 401:
                realm, nonce, qop = parse_chal(rsp)
                auth = auth_hdr(self.impi, realm, self.pw, "REGISTER", uri, nonce, qop); continue
            break
        return code, rsp
    def recv_udp(self):
        d, a = self.udp.recvfrom(65535); return d.decode(errors="replace"), a
    def reply_200(self, req, addr):
        keep = [l for l in req.split("\r\n")[1:] if re.match(r"(Via|From|To|Call-ID|CSeq):", l, re.I)]
        keep = [(l + f";tag={uuid.uuid4().hex[:8]}") if l.lower().startswith("to:") and "tag=" not in l else l for l in keep]
        self.udp.sendto(("\r\n".join(["SIP/2.0 200 OK"] + keep) + "\r\nContent-Length: 0\r\n\r\n").encode(), addr)
    def send_message_udp(self, to_num, ct, body):
        uri = f"sip:{to_num}@{DOMAIN}"; tag = uuid.uuid4().hex[:8]; callid = str(uuid.uuid4())
        h = [f"MESSAGE {uri} SIP/2.0",
             f"Via: SIP/2.0/UDP {LOCAL_IP}:{self.port};rport;branch=z9hG4bK{uuid.uuid4().hex[:12]}",
             "Max-Forwards: 70", f"From: <{self.aor()}>;tag={tag}", f"To: <{uri}>", f"Call-ID: {callid}",
             "CSeq: 5 MESSAGE", f"Content-Type: {ct}", f"Content-Length: {len(body.encode())}"]
        self.udp.sendto(("\r\n".join(h) + "\r\n\r\n" + body).encode(), (SERVER, PORT))

def mcdata_sds_body(target_uri, text):
    """앱과 같은 one-to-one-sds multipart (TLV base64)."""
    conv = uuid.uuid4().bytes; mid = uuid.uuid4().bytes
    # Date-time 5B(UTC 초) — 상위 1B 0 + 4B
    sig = bytes([0x01]) + b"\x00" + struct.pack(">I", int(time.time())) + conv + mid + bytes([0x81])
    t = text.encode(); pay = bytes([0x03, 0x01, 0x78]) + struct.pack(">H", 1 + len(t)) + bytes([0x01]) + t
    b = "mcdata-" + uuid.uuid4().hex[:16]
    info = ('<?xml version="1.0" encoding="UTF-8"?>\n<mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0">\n'
            '  <mcdata-Params>\n    <request-type>one-to-one-sds</request-type>\n'
            f'    <mcdata-request-uri type="Normal"><mcdataURI>{target_uri}</mcdataURI></mcdata-request-uri>\n'
            '  </mcdata-Params>\n</mcdatainfo>')
    body = (f"--{b}\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n{info}\r\n"
            f"--{b}\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\nContent-Transfer-Encoding: base64\r\n\r\n{base64.b64encode(sig).decode()}\r\n"
            f"--{b}\r\nContent-Type: application/vnd.3gpp.mcdata-payload\r\nContent-Transfer-Encoding: base64\r\n\r\n{base64.b64encode(pay).decode()}\r\n"
            f"--{b}--\r\n")
    return f"multipart/mixed;boundary={b}", body

def send_message_tcp(ua, to_num, ct, body):
    s = socket.create_connection((SERVER, PORT), timeout=6); s.settimeout(8)
    lport = s.getsockname()[1]; uri = f"sip:{to_num}@{DOMAIN}"; callid = str(uuid.uuid4())
    tag = uuid.uuid4().hex[:8]; cseq = 100; auth = None; codes = []; rsp = ""
    for _ in range(3):
        cseq += 1
        h = [f"MESSAGE {uri} SIP/2.0",
             f"Via: SIP/2.0/TCP {LOCAL_IP}:{lport};rport;branch=z9hG4bK{uuid.uuid4().hex[:12]}",
             "Max-Forwards: 70", f"From: <{ua.aor()}>;tag={tag}", f"To: <{uri}>", f"Call-ID: {callid}",
             f"CSeq: {cseq} MESSAGE", "User-Agent: cims-premise-test",
             f"Content-Type: {ct}", f"Content-Length: {len(body.encode())}"]
        if auth: h.append(auth)
        wire = ("\r\n".join(h) + "\r\n\r\n" + body).encode()
        print(f"  TCP MESSAGE #{len(codes)+1}: {len(wire)} bytes, auth={'yes' if auth else 'no'}")
        s.sendall(wire)
        rsp = recv_sip(s.recv); code = status(rsp); codes.append(code)
        if code == 401:
            realm, nonce, qop = parse_chal(rsp)
            auth = auth_hdr(ua.impi, realm, ua.pw, "MESSAGE", uri, nonce, qop); continue
        break
    s.close(); return codes, rsp

def main():
    global SERVER, LOCAL_IP, DOMAIN
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="11"); ap.add_argument("--b", default="12")
    ap.add_argument("--password", default="123456", help="시험 가입자 SIP Digest 비밀번호")
    ap.add_argument("--ip", default=SERVER, help="CSP 주소 (= 이 스크립트가 광고할 로컬 주소, 서버 로컬 실행 전제)")
    ap.add_argument("--domain", default=DOMAIN)
    o = ap.parse_args()
    SERVER = LOCAL_IP = o.ip; DOMAIN = o.domain
    A = Ua(f"+825000000{o.a}", f"45033825000000{o.a}", o.password, 45011)
    B = Ua(f"+825000000{o.b}", f"45033825000000{o.b}", o.password, 45012)
    okc = True
    for ua in (A, B):
        code, rsp = ua.register()
        print(f"[1] REGISTER {ua.num} (UDP) → {code}"); okc &= code == 200
    if not okc: print("  !! 등록 실패 — 비밀번호/계정 확인"); return 1

    ct, body = mcdata_sds_body(f"tel:{B.num}", "TCP 승격 재인증 검증 " + "x" * 300)
    codes, rsp = send_message_tcp(A, B.num, ct, body)
    print(f"[2] A→B MESSAGE over TCP: responses {codes}")
    ok2 = codes[:1] == [401] and codes[-1] in (200, 202)
    print(f"  {'PASS' if ok2 else 'FAIL'}: 401 챌린지 → 재발행 → {codes[-1]}")

    try:
        req, addr = B.recv_udp()
        got = req.startswith("MESSAGE ") and "one-to-one-sds" in req and body.split("\r\n")[-3] in req
        B.reply_200(req, addr)
        print(f"[3] B UDP 수신: {req.splitlines()[0]} from {addr} — {'PASS' if got else 'FAIL'} (본문 일치)")
    except socket.timeout:
        got = False; print("[3] B UDP 수신: FAIL (timeout)")

    B.send_message_udp(A.num, "text/plain", "ping-after-tcp-auth")
    rsp, _ = B.recv_udp(); print(f"[4] B→A MESSAGE over UDP → {status(rsp)}")
    try:
        req, addr = A.recv_udp()
        ok4 = req.startswith("MESSAGE ") and "ping-after-tcp-auth" in req
        A.reply_200(req, addr)
        print(f"  A UDP 수신: {req.splitlines()[0]} from {addr} — {'PASS' if ok4 else 'FAIL'} (UDP 바인딩 무손상)")
    except socket.timeout:
        ok4 = False; print("  A UDP 수신: FAIL (timeout — 바인딩 손상 의심)")

    for ua in (A, B):
        code, _ = ua.register(expires=0); print(f"[5] REGISTER {ua.num} Expires:0 → {code}")
    allok = ok2 and got and ok4
    print("RESULT:", "PASS" if allok else "FAIL"); return 0 if allok else 2

if __name__ == "__main__":
    sys.exit(main())
