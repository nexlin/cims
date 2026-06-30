#!/usr/bin/env python3
"""
F-07 Stale Nonce 검증 테스트
TS 24.229 §5.4.1.2 — 소비된 nonce 로 REGISTER 시 401 stale=true 응답 확인

흐름:
  Step 1  REGISTER (no auth)           → 401 + nonce N1 발급
  Step 2  REGISTER + Digest(N1)        → 200 OK  (N1 소비: NonceMap.Select bDelete=true)
  Step 3  REGISTER + Digest(N1 재사용) → 401 stale=true  ← F-07 검증 포인트

왜 TTL 조정 없이 가능한가:
  NonceMap.cpp Select(bDelete=true): nonce 는 첫 CheckAuthorization 호출 즉시 삭제.
  200 OK 후 같은 nonce 를 재사용하면 E_AUTH_NONCE_NOT_FOUND → stale=true 경로.

사용법:
  python3 tests/f07_stale_nonce.py
  python3 tests/f07_stale_nonce.py --ip 121.134.202.23 --port 5160
  python3 tests/f07_stale_nonce.py --imsi 001011000000001 --domain csp --password 1234
"""

import argparse
import hashlib
import re
import socket
import sys
import uuid


# ── 색상 ───────────────────────────────────────────────────────────────────
GRN = "\033[32m"
RED = "\033[31m"
YLW = "\033[1;33m"
CYN = "\033[36m"
NC  = "\033[0m"

def ok(msg):   print(f"  {GRN}[PASS]{NC} {msg}")
def fail(msg): print(f"  {RED}[FAIL]{NC} {msg}")
def info(msg): print(f"  {CYN}[INFO]{NC} {msg}")
def warn(msg): print(f"  {YLW}[WARN]{NC} {msg}")


# ── Digest 계산 ────────────────────────────────────────────────────────────

def _md5(s: str) -> str:
    return hashlib.md5(s.encode()).hexdigest()

def digest_response(username: str, realm: str, password: str,
                    method: str, uri: str, nonce: str,
                    qop: str = "auth", cnonce: str = "1",
                    nc: str = "00000001") -> str:
    ha1 = _md5(f"{username}:{realm}:{password}")
    ha2 = _md5(f"{method}:{uri}")
    if qop == "auth":
        return _md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
    return _md5(f"{ha1}:{nonce}:{ha2}")


# ── SIP 메시지 빌더 ────────────────────────────────────────────────────────

def make_register(local_ip: str, local_port: int,
                  user: str, domain: str, cseq: int,
                  call_id: str, from_tag: str,
                  auth_header: str = "") -> bytes:
    branch = "z9hG4bK" + uuid.uuid4().hex[:10]
    lines = [
        f"REGISTER sip:{domain} SIP/2.0",
        f"Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport",
        f"From: <sip:{user}@{domain}>;tag={from_tag}",
        f"To: <sip:{user}@{domain}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} REGISTER",
        f"Contact: <sip:{user}@{local_ip}:{local_port}>",
        "Max-Forwards: 70",
        "Expires: 600",
        "User-Agent: F07-StaleNonce-Test/1.0",
    ]
    if auth_header:
        lines.append(auth_header)
    lines += ["Content-Length: 0", "", ""]
    return "\r\n".join(lines).encode()

def make_auth_header(username: str, realm: str, password: str,
                     nonce: str, uri: str,
                     qop: str = "auth", cnonce: str = "1",
                     nc: str = "00000001") -> str:
    resp = digest_response(username, realm, password,
                           "REGISTER", uri, nonce, qop, cnonce, nc)
    return (f'Authorization: Digest username="{username}", realm="{realm}", '
            f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=MD5, '
            f'cnonce="{cnonce}", qop={qop}, nc={nc}')


# ── UDP 송수신 ─────────────────────────────────────────────────────────────

def send_recv(sock: socket.socket, msg: bytes,
              server_ip: str, server_port: int,
              timeout: float = 3.0) -> str:
    sock.settimeout(timeout)
    sock.sendto(msg, (server_ip, server_port))
    try:
        data, _ = sock.recvfrom(4096)
        return data.decode(errors="replace")
    except socket.timeout:
        return ""

def parse_status(resp: str) -> int:
    m = re.match(r"SIP/2\.0\s+(\d{3})", resp)
    return int(m.group(1)) if m else 0

def extract_nonce(resp: str) -> str:
    m = re.search(r'nonce="([^"]+)"', resp, re.IGNORECASE)
    return m.group(1) if m else ""

def extract_realm(resp: str) -> str:
    m = re.search(r'realm="([^"]+)"', resp, re.IGNORECASE)
    return m.group(1) if m else ""

def has_stale_true(resp: str) -> bool:
    return bool(re.search(r'stale\s*=\s*["\']?true["\']?', resp, re.IGNORECASE))

def www_auth_line(resp: str) -> str:
    for line in resp.splitlines():
        if re.match(r'www-authenticate', line, re.IGNORECASE):
            return line.strip()
    return ""


# ── 테스트 ─────────────────────────────────────────────────────────────────

def run(server_ip: str, server_port: int,
        user: str, imsi: str, domain: str, password: str) -> bool:

    username = f"{imsi}@{domain}"   # Digest username = IMSI@domain
    uri      = f"sip:{domain}"

    print(f"\n{'='*62}")
    print(f"  F-07 Stale Nonce Test")
    print(f"  CSP    : {server_ip}:{server_port}")
    print(f"  SIP ID : sip:{user}@{domain}")
    print(f"  Digest : username={username}")
    print(f"{'='*62}\n")

    local_port = 51999
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", local_port))

    local_ip = server_ip
    call_id  = f"{uuid.uuid4().hex}@{local_ip}"
    from_tag = uuid.uuid4().hex[:8]
    cseq     = 1
    passed   = True

    try:
        # ── Step 1: REGISTER (no auth) → 401 ──────────────────────────────
        print("  Step 1  REGISTER (인증 없음) → 401 + nonce 수신")
        msg  = make_register(local_ip, local_port, user, domain,
                             cseq, call_id, from_tag)
        resp = send_recv(sock, msg, server_ip, server_port)
        if not resp:
            fail("CSP 응답 없음 — IP/Port 확인 필요")
            return False

        status = parse_status(resp)
        nonce  = extract_nonce(resp)
        realm  = extract_realm(resp) or domain
        info(f"응답: {status}  realm={realm}  nonce={nonce[:20]}…")

        if status != 401:
            fail(f"기대값 401, 실제 {status}")
            return False
        if not nonce:
            fail("401 응답에 nonce 없음")
            return False
        ok("401 + nonce 정상 수신")

        # ── Step 2: Digest(N1) → 200 OK  (N1 소비) ───────────────────────
        cseq += 1
        print("\n  Step 2  REGISTER + Digest(N1) → 200 OK  (nonce 소비)")
        auth = make_auth_header(username, realm, password, nonce, uri)
        msg  = make_register(local_ip, local_port, user, domain,
                             cseq, call_id, from_tag, auth)
        resp = send_recv(sock, msg, server_ip, server_port)
        status2 = parse_status(resp)
        info(f"응답: {status2}")

        if status2 == 200:
            ok("200 OK — nonce N1 소비 완료")
        elif status2 == 401:
            warn(f"재챌린지 401 (패스워드·username 불일치 가능성)")
            warn(f"  → {www_auth_line(resp)}")
            warn("  Step 3 는 원래 N1 으로 계속 진행")
        else:
            fail(f"기대값 200, 실제 {status2}")
            passed = False

        # ── Step 3: N1 재사용 → 401 stale=true ───────────────────────────
        cseq += 1
        print("\n  Step 3  REGISTER + Digest(N1 재사용) → 401 stale=true 확인")
        info(f"재사용 nonce: {nonce[:20]}…")
        auth = make_auth_header(username, realm, password, nonce, uri)
        msg  = make_register(local_ip, local_port, user, domain,
                             cseq, call_id, from_tag, auth)
        resp = send_recv(sock, msg, server_ip, server_port)
        status3 = parse_status(resp)
        stale   = has_stale_true(resp)
        wwa     = www_auth_line(resp)
        info(f"응답: {status3}  stale={stale}")
        if wwa:
            info(f"{wwa}")

        if status3 == 401 and stale:
            ok("401 + stale=true 확인 → F-07 PASS")
        elif status3 == 401 and not stale:
            fail("401 이지만 stale=true 없음 → F-07 FAIL")
            passed = False
        else:
            fail(f"기대값 401+stale, 실제 {status3} → F-07 FAIL")
            passed = False

    finally:
        sock.close()

    print(f"\n{'='*62}")
    if passed:
        print(f"  {GRN}결과: PASS{NC}  F-07 stale nonce 재챌린지 정상 동작")
    else:
        print(f"  {RED}결과: FAIL{NC}  F-07 검증 실패")
    print(f"{'='*62}\n")
    return passed


# ── CLI ───────────────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(description="F-07 Stale Nonce 검증 테스트")
    p.add_argument("--ip",       default="121.134.202.23",
                   help="CSP IP (기본: 121.134.202.23)")
    p.add_argument("--port",     default=5160, type=int,
                   help="CSP SIP UDP 포트 (기본: 5160)")
    p.add_argument("--user",     default="1001",
                   help="SIP user (From URI, 기본: 1001)")
    p.add_argument("--imsi",     default="001011000000001",
                   help="Digest username 용 IMSI (기본: 001011000000001)")
    p.add_argument("--domain",   default="csp",
                   help="SIP domain / realm (기본: csp)")
    p.add_argument("--password", default="1234",
                   help="비밀번호 (기본: 1234)")
    args = p.parse_args()

    ok_flag = run(args.ip, args.port,
                  args.user, args.imsi, args.domain, args.password)
    sys.exit(0 if ok_flag else 1)


if __name__ == "__main__":
    main()
