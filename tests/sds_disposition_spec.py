#!/usr/bin/env python3
"""SDS disposition 통지의 규격 경로 서버 계약 검증 (TS 24.282 V18.13.0 §12.2.1.1·§12.2.2.1·§12.2.3, mcdata_messaging.md §4.4).

단말이 수신 SDS 의 DELIVERED 통지를 Request-URI = MCData 참여 기능 PSI 로, 대상 MCData ID 를 application/resource-lists+xml
(entry 하나)로 보내면, CSP(참여·제어 기능 겸)는 원 SDS 와 대화·메시지 ID 로 상관한 뒤 원 발신자에게 새 MESSAGE 로 중계한다 —
mcdata-info <mcdata-request-uri> = 원 발신자, <mcdata-calling-user-id> = 통지자, mcdata-signalling 파트는 받은 그대로.

사용법 (CSP 가 도는 서버에서 — H(A1) 은 DB 의 ptt_subscriptions.ha1):
  python3 tests/sds_disposition_spec.py --ip 121.161.164.48 \
      --a +82500000013:4503382500000013:<ha1> --b +82500000014:4503382500000014:<ha1>

  A·B UDP REGISTER → 200
  A ── 1:1 SDS(disposition 요청 DELIVERED) ──→ B 수신 → 200
  B ── SDS NOTIFICATION(DELIVERED) → sip:mcdata_psi@도메인, resource-lists [A] ──→ 200, A 가 중계 MESSAGE 수신(본문 검증)
  B ── resource-lists 두 entry ──→ 403 Warning 145
  B ── 모르는 메시지 ID ──→ 403 Warning 216
  B ── Accept-Contact ICSI 없음 ──→ 403
  A/B REGISTER Expires:0
"""
import argparse, base64, hashlib, re, socket, struct, sys, time, uuid

SERVER = "121.161.164.48"; PORT = 15060
DOMAIN = "ptt.cims.example.kr"
LOCAL_IP = "121.161.164.48"
ICSI_SDS = "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds"

def md5(s): return hashlib.md5(s.encode()).hexdigest()
def auth_hdr(impi, realm, ha1, method, uri, nonce, qop):
    ha2 = md5(f"{method}:{uri}")
    r = md5(f"{ha1}:{nonce}:00000001:abc:{qop}:{ha2}") if qop else md5(f"{ha1}:{nonce}:{ha2}")
    h = f'Authorization: Digest username="{impi}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{r}", algorithm=MD5'
    if qop: h += f', cnonce="abc", qop={qop}, nc=00000001'
    return h
def parse_chal(msg):
    m = re.search(r"WWW-Authenticate:\s*Digest\s+(.*)", msg, re.I)
    d = dict(re.findall(r'(\w+)="?([^",]+)"?', m.group(1)))
    return d["realm"], d["nonce"], d.get("qop")
def status(msg): return int(msg.split(" ", 2)[1])
def hdr(msg, name):
    m = re.search(rf"^{name}:\s*(.*)$", msg, re.I | re.M); return m.group(1).strip() if m else ""

class Ua:
    def __init__(self, num, imsi, ha1, port):
        self.num, self.impi, self.ha1, self.port = num, f"{imsi}@{DOMAIN}", ha1, port
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
                 f"Contact: <sip:{self.num}@{LOCAL_IP}:{self.port}>;+g.3gpp.mcdata.sds", f"Expires: {expires}",
                 "User-Agent: cims-disposition-test"]
            if auth: h.append(auth)
            self.udp.sendto(("\r\n".join(h) + "\r\nContent-Length: 0\r\n\r\n").encode(), (SERVER, PORT))
            rsp = self.recv(); code = status(rsp)
            if code == 401:
                realm, nonce, qop = parse_chal(rsp)
                auth = auth_hdr(self.impi, realm, self.ha1, "REGISTER", uri, nonce, qop); continue
            break
        return code, rsp
    def recv(self, method=None, timeout=6):
        """다음 SIP 메시지(method 가 있으면 그 요청이 올 때까지 — 다른 요청엔 200)."""
        end = time.time() + timeout
        while time.time() < end:
            self.udp.settimeout(max(0.1, end - time.time()))
            try: d, a = self.udp.recvfrom(65535)
            except socket.timeout: break
            m = d.decode(errors="replace")
            if method is None: return m
            if m.startswith(method + " "): self.reply_200(m, a); return m
            if not m.startswith("SIP/2.0"): self.reply_200(m, a)
        raise TimeoutError(method or "response")
    def reply_200(self, req, addr):
        keep = [l for l in req.split("\r\n")[1:] if re.match(r"(Via|From|To|Call-ID|CSeq):", l, re.I)]
        keep = [(l + f";tag={uuid.uuid4().hex[:8]}") if l.lower().startswith("to:") and "tag=" not in l else l for l in keep]
        self.udp.sendto(("\r\n".join(["SIP/2.0 200 OK"] + keep) + "\r\nContent-Length: 0\r\n\r\n").encode(), addr)
    def message(self, ruri, ct, body, extra=()):
        tag = uuid.uuid4().hex[:8]; callid = str(uuid.uuid4())
        h = [f"MESSAGE {ruri} SIP/2.0",
             f"Via: SIP/2.0/UDP {LOCAL_IP}:{self.port};rport;branch=z9hG4bK{uuid.uuid4().hex[:12]}",
             "Max-Forwards: 70", f"From: <{self.aor()}>;tag={tag}", f"To: <{ruri}>", f"Call-ID: {callid}",
             "CSeq: 5 MESSAGE", *extra, f"Content-Type: {ct}", f"Content-Length: {len(body.encode())}"]
        self.udp.sendto(("\r\n".join(h) + "\r\n\r\n" + body).encode(), (SERVER, PORT))
        while True:
            m = self.recv()
            if m.startswith("SIP/2.0") and status(m) >= 200 and hdr(m, "Call-ID") == callid: return m

def b64part(ct, raw):
    return f"Content-Type: {ct}\r\nContent-Transfer-Encoding: base64\r\n\r\n{base64.b64encode(raw).decode()}"

def sds_body(target, conv, mid, text):
    """1:1 SDS — SDS SIGNALLING PAYLOAD(§15.1.2, disposition 요청 DELIVERED 0x81) + DATA PAYLOAD(TEXT)."""
    sig = bytes([0x01]) + b"\x00" + struct.pack(">I", int(time.time())) + conv + mid + bytes([0x81])
    t = text.encode(); pay = bytes([0x03, 0x01, 0x78]) + struct.pack(">H", 1 + len(t)) + bytes([0x01]) + t
    b = "mcdata-" + uuid.uuid4().hex[:16]
    info = ('<?xml version="1.0" encoding="UTF-8"?>\n<mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0">\n  <mcdata-Params>\n'
            '    <request-type>one-to-one-sds</request-type>\n'
            f'    <mcdata-request-uri type="Normal"><mcdataURI>tel:{target}</mcdataURI></mcdata-request-uri>\n'
            '  </mcdata-Params>\n</mcdatainfo>')
    body = (f"--{b}\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n{info}\r\n"
            f"--{b}\r\n{b64part('application/vnd.3gpp.mcdata-signalling', sig)}\r\n"
            f"--{b}\r\n{b64part('application/vnd.3gpp.mcdata-payload', pay)}\r\n--{b}--\r\n")
    return f"multipart/mixed;boundary={b}", body

def notification_body(targets, conv, mid, group=None):
    """SDS NOTIFICATION(§15.1.5 — DELIVERED 0x02) + resource-lists(§12.2.1.1 3)) [+ mcdata-calling-group-id 5)]."""
    sig = bytes([0x05, 0x02]) + b"\x00" + struct.pack(">I", int(time.time())) + conv + mid
    b = "mcdata-" + uuid.uuid4().hex[:16]
    entries = "".join(f'<entry uri="{t}"/>' for t in targets)
    rl = ('<?xml version="1.0" encoding="UTF-8"?>\n<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists">'
          f'<list>{entries}</list></resource-lists>')
    parts = []
    if group:
        parts.append('Content-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0"><mcdata-Params>'
                     f'<mcdata-calling-group-id type="Normal"><mcdataURI>tel:{group}</mcdataURI></mcdata-calling-group-id>'
                     '</mcdata-Params></mcdatainfo>')
    parts.append(b64part("application/vnd.3gpp.mcdata-signalling", sig))
    parts.append(f"Content-Type: application/resource-lists+xml\r\nContent-Disposition: recipient-list\r\n\r\n{rl}")
    body = "".join(f"--{b}\r\n{p}\r\n" for p in parts) + f"--{b}--\r\n"
    return f"multipart/mixed;boundary={b}", body, sig

SDS_HEADERS = (f"Accept-Contact: *;+g.3gpp.mcdata.sds;require;explicit",
               f'Accept-Contact: *;+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds";require;explicit',
               f"P-Preferred-Service: {ICSI_SDS}")

def main():
    global SERVER, LOCAL_IP
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=SERVER); ap.add_argument("--a", required=True); ap.add_argument("--b", required=True)
    o = ap.parse_args(); SERVER = LOCAL_IP = o.ip
    A = Ua(*o.a.split(":"), 26013); B = Ua(*o.b.split(":"), 26014)
    fails = 0
    def check(name, ok, extra=""):
        nonlocal fails
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' — ' + extra) if extra else ''}"); fails += 0 if ok else 1
    try:
        for u in (A, B):
            c, _ = u.register(); check(f"REGISTER {u.num}", c == 200, str(c))
        conv, mid = uuid.uuid4().bytes, uuid.uuid4().bytes
        ct, body = sds_body(B.num, conv, mid, "disposition spec test")
        r = A.message(f"sip:{B.num}@{DOMAIN}", ct, body, SDS_HEADERS)
        check("A→B 1:1 SDS", status(r) == 200, str(status(r)))
        B.recv("MESSAGE")
        psi = f"sip:mcdata_psi@{DOMAIN}"
        ct, body, sig = notification_body([f"tel:{A.num}"], conv, mid)
        r = B.message(psi, ct, body, SDS_HEADERS)
        check("B→PSI DELIVERED(resource-lists [A])", status(r) == 200, str(status(r)))
        m = A.recv("MESSAGE")
        info = m[m.find("mcdatainfo"):] if "mcdatainfo" in m else ""
        check("A 중계 수신 mcdata-request-uri = A", f"<mcdataURI>tel:{A.num}</mcdataURI></mcdata-request-uri>" in info)
        check("A 중계 수신 mcdata-calling-user-id = B", f"<mcdataURI>tel:{B.num}</mcdataURI></mcdata-calling-user-id>" in info)
        check("A 중계 수신 signalling 원문", base64.b64encode(sig).decode() in m)
        check("A 중계 수신 P-Asserted-Service", hdr(m, "P-Asserted-Service") == ICSI_SDS, hdr(m, "P-Asserted-Service"))
        ct, body, _ = notification_body([f"tel:{A.num}", "tel:+82500000099"], conv, mid)
        r = B.message(psi, ct, body, SDS_HEADERS)
        check("entry 둘 → 403 Warning 145", status(r) == 403 and '"145 ' in hdr(r, "Warning"), hdr(r, "Warning"))
        ct, body, _ = notification_body([f"tel:{A.num}"], conv, uuid.uuid4().bytes)
        r = B.message(psi, ct, body, SDS_HEADERS)
        check("모르는 메시지 ID → 403 Warning 216", status(r) == 403 and '"216 ' in hdr(r, "Warning"), hdr(r, "Warning"))
        ct, body, _ = notification_body([f"tel:{A.num}"], conv, mid)
        r = B.message(psi, ct, body, (f"P-Preferred-Service: {ICSI_SDS}",))
        check("Accept-Contact ICSI 없음 → 403", status(r) == 403, str(status(r)))
    finally:
        for u in (A, B):
            try: u.register(expires=0)
            except Exception: pass
    print("PASS" if fails == 0 else f"FAIL ({fails})"); return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
