#!/usr/bin/env python3
"""
F-06 미등록 사용자 SUBSCRIBE 검증 테스트
RFC 3261 §22 — 미인증 SUBSCRIBE 에 403 대신 401 + WWW-Authenticate 응답 확인

흐름:
  Step 1  SUBSCRIBE (Authorization 없음)        → 401 + WWW-Authenticate 확인  ← F-06 핵심
  Step 2  SUBSCRIBE + Digest 인증               → 200 OK (정상 구독 가능 확인)
  Step 3  존재하지 않는 사용자로 SUBSCRIBE        → 403 확인  (올바른 거부 경로)

왜 이게 중요한가:
  표준 단말은 403 = 영구 거부로 처리 → 재시도 안 함 → 그룹 목록 미갱신.
  401 = 인증 필요 → 단말이 REGISTER 후 재시도 → 정상 복구.
  단말 제조사에 따라 SUBSCRIBE 를 REGISTER 보다 먼저 보내는 경우가 있어
  이 경로가 실제로 밟힐 수 있음.

사용법:
  python3 tests/f06_unregistered_subscribe.py
  python3 tests/f06_unregistered_subscribe.py --ip 121.134.202.23 --port 5160
  python3 tests/f06_unregistered_subscribe.py --user 1001 --imsi 001011000000001 --password 1234
  python3 tests/f06_unregistered_subscribe.py --unknown-user 9999  # Step 3 용 미존재 사용자
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

def make_auth_header(username: str, realm: str, password: str,
                     nonce: str, uri: str,
                     method: str = "SUBSCRIBE",
                     qop: str = "auth", cnonce: str = "1",
                     nc: str = "00000001") -> str:
    resp = digest_response(username, realm, password, method, uri,
                           nonce, qop, cnonce, nc)
    return (f'Authorization: Digest username="{username}", realm="{realm}", '
            f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=MD5, '
            f'cnonce="{cnonce}", qop={qop}, nc={nc}')


# ── SIP 메시지 빌더 ────────────────────────────────────────────────────────

def make_subscribe(local_ip: str, local_port: int,
                   user: str, domain: str, event: str,
                   cseq: int, call_id: str, from_tag: str,
                   auth_header: str = "") -> bytes:
    branch  = "z9hG4bK" + uuid.uuid4().hex[:10]
    psi_uri = f"sip:{event}@{domain}"
    lines = [
        f"SUBSCRIBE {psi_uri} SIP/2.0",
        f"Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport",
        f"From: <sip:{user}@{domain}>;tag={from_tag}",
        f"To: <{psi_uri}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} SUBSCRIBE",
        f"Contact: <sip:{user}@{local_ip}:{local_port}>",
        "Max-Forwards: 70",
        "Event: xcap-diff",
        "Expires: 3600",
        "Accept: application/xcap-diff+xml",
        "User-Agent: F06-UnregSubscribe-Test/1.0",
    ]
    if auth_header:
        lines.append(auth_header)
    lines += ["Content-Length: 0", "", ""]
    return "\r\n".join(lines).encode()


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

def has_www_auth(resp: str) -> bool:
    return bool(re.search(r'^www-authenticate', resp, re.IGNORECASE | re.MULTILINE))

def www_auth_line(resp: str) -> str:
    for line in resp.splitlines():
        if re.match(r'www-authenticate', line, re.IGNORECASE):
            return line.strip()
    return ""


# ── 테스트 ─────────────────────────────────────────────────────────────────

def run(server_ip: str, server_port: int,
        user: str, imsi: str, domain: str, password: str,
        unknown_user: str) -> bool:

    username  = f"{imsi}@{domain}"
    event_psi = "gms_psi"
    uri       = f"sip:{event_psi}@{domain}"

    print(f"\n{'='*62}")
    print(f"  F-06 Unregistered SUBSCRIBE Test")
    print(f"  CSP    : {server_ip}:{server_port}")
    print(f"  User   : sip:{user}@{domain}  (REGISTER 없이 바로 SUBSCRIBE)")
    print(f"  Digest : username={username}")
    print(f"{'='*62}\n")

    local_port = 51998
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", local_port))

    local_ip = server_ip
    call_id  = f"{uuid.uuid4().hex}@{local_ip}"
    from_tag = uuid.uuid4().hex[:8]
    cseq     = 1
    passed   = True

    try:
        # ── Step 1: SUBSCRIBE (no auth) → 401 + WWW-Authenticate ─────────
        print("  Step 1  SUBSCRIBE (인증 없음) → 401 + WWW-Authenticate 확인")
        print("          (REGISTER 없이 바로 SUBSCRIBE — 백그라운드 복귀 단말 시뮬)")
        msg  = make_subscribe(local_ip, local_port, user, domain,
                              event_psi, cseq, call_id, from_tag)
        resp = send_recv(sock, msg, server_ip, server_port)
        if not resp:
            fail("CSP 응답 없음 — IP/Port 확인 필요")
            return False

        status = parse_status(resp)
        has_auth = has_www_auth(resp)
        nonce  = extract_nonce(resp)
        realm  = extract_realm(resp) or domain
        info(f"응답: {status}  WWW-Authenticate: {has_auth}  nonce={nonce[:16]}…" if nonce else
             f"응답: {status}  WWW-Authenticate: {has_auth}")

        if status == 401 and has_auth:
            ok("401 + WWW-Authenticate 확인 → F-06 PASS (403이 아닌 401 응답)")
            if www_auth_line(resp):
                info(www_auth_line(resp))
        elif status == 403:
            fail("403 Forbidden → F-06 FAIL (단말은 이걸 영구 거부로 처리, 재시도 안 함)")
            passed = False
        elif status == 401 and not has_auth:
            fail("401이지만 WWW-Authenticate 없음 → 단말이 재시도 불가")
            passed = False
        else:
            fail(f"기대값 401, 실제 {status}")
            passed = False

        if not nonce:
            warn("nonce 없음 — Step 2 건너뜀")
        else:
            # ── Step 2: Digest 인증 → 200 OK ─────────────────────────────
            cseq += 1
            print(f"\n  Step 2  SUBSCRIBE + Digest 인증 → 200 OK (정상 구독 확인)")
            auth = make_auth_header(username, realm, password, nonce, uri)
            call_id2 = f"{uuid.uuid4().hex}@{local_ip}"
            from_tag2 = uuid.uuid4().hex[:8]
            msg  = make_subscribe(local_ip, local_port, user, domain,
                                  event_psi, cseq, call_id2, from_tag2, auth)
            resp = send_recv(sock, msg, server_ip, server_port)
            status2 = parse_status(resp)
            info(f"응답: {status2}")

            if status2 == 200:
                ok("200 OK — Digest 인증 후 구독 성공")
            elif status2 == 401:
                warn(f"재챌린지 401 (username·password 불일치 가능성)")
                warn(f"  → {www_auth_line(resp)}")
            else:
                fail(f"기대값 200, 실제 {status2}")
                passed = False

        # ── Step 3: 존재하지 않는 사용자 → 403 ──────────────────────────
        cseq += 1
        print(f"\n  Step 3  미존재 사용자({unknown_user}) SUBSCRIBE → 403 확인")
        print(f"          (계정 자체가 없는 경우는 403이 올바른 응답)")
        bad_call_id  = f"{uuid.uuid4().hex}@{local_ip}"
        bad_from_tag = uuid.uuid4().hex[:8]
        msg  = make_subscribe(local_ip, local_port, unknown_user, domain,
                              event_psi, cseq, bad_call_id, bad_from_tag)
        resp = send_recv(sock, msg, server_ip, server_port)
        status3 = parse_status(resp)
        info(f"응답: {status3}")

        if status3 == 403:
            ok("403 Forbidden — 미존재 계정은 올바르게 거부")
        elif status3 == 401:
            warn("401 응답 (미존재 사용자도 챌린지 발급 — CSP 구현 방식에 따라 허용)")
        else:
            warn(f"응답: {status3} (예상과 다르지만 치명적이지 않음)")

    finally:
        sock.close()

    print(f"\n{'='*62}")
    if passed:
        print(f"  {GRN}결과: PASS{NC}  F-06 미등록 SUBSCRIBE → 401 정상 응답")
    else:
        print(f"  {RED}결과: FAIL{NC}  F-06 검증 실패")
    print(f"{'='*62}\n")
    return passed


# ── CLI ───────────────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(description="F-06 미등록 SUBSCRIBE 검증 테스트")
    p.add_argument("--ip",           default="121.134.202.23",
                   help="CSP IP (기본: 121.134.202.23)")
    p.add_argument("--port",         default=5160, type=int,
                   help="CSP SIP UDP 포트 (기본: 5160)")
    p.add_argument("--user",         default="1001",
                   help="SIP user (기본: 1001)")
    p.add_argument("--imsi",         default="001011000000001",
                   help="Digest username 용 IMSI (기본: 001011000000001)")
    p.add_argument("--domain",       default="csp",
                   help="SIP domain / realm (기본: csp)")
    p.add_argument("--password",     default="1234",
                   help="비밀번호 (기본: 1234)")
    p.add_argument("--unknown-user", default="9999",
                   help="Step 3 미존재 사용자 ID (기본: 9999)")
    args = p.parse_args()

    ok_flag = run(args.ip, args.port,
                  args.user, args.imsi, args.domain, args.password,
                  args.unknown_user)
    sys.exit(0 if ok_flag else 1)


if __name__ == "__main__":
    main()
