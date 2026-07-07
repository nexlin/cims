#!/usr/bin/env python3
"""
F-13 SIP-If-Match 검증 테스트
RFC 3903 §4 — re-PUBLISH 시 If-Match 불일치 → 412 Precondition Failed 확인

흐름:
  Step 1  REGISTER (no auth)              → 401 + nonce 수신
  Step 2  REGISTER + Digest               → 200 OK (등록 완료)
  Step 3  PUBLISH (no auth)               → 401 + nonce 수신
  Step 4  PUBLISH + Digest                → 200 OK + SIP-ETag 수령
  Step 5  re-PUBLISH + If-Match: 엉뚱한값  → 412  ← F-13 핵심
  Step 6  re-PUBLISH + If-Match: 올바른값  → 200 OK (정상 갱신)

실제 단말 상황:
  앱 충돌/재시작 후 잘못된 ETag로 re-PUBLISH → CSP가 412로 거부
  → 단말이 최초 PUBLISH부터 다시 시작 → 정상 복구

사용법:
  python3 tests/f13_if_match.py
  python3 tests/f13_if_match.py --ip 121.134.202.23 --port 5160
  python3 tests/f13_if_match.py --user 1001 --imsi 001011000000001 --password 1234
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
                     nonce: str, uri: str, method: str = "REGISTER",
                     qop: str = "auth", cnonce: str = "1",
                     nc: str = "00000001") -> str:
    resp = digest_response(username, realm, password, method, uri,
                           nonce, qop, cnonce, nc)
    return (f'Authorization: Digest username="{username}", realm="{realm}", '
            f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=MD5, '
            f'cnonce="{cnonce}", qop={qop}, nc={nc}')


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
        "User-Agent: F13-IfMatch-Test/1.0",
    ]
    if auth_header:
        lines.append(auth_header)
    lines += ["Content-Length: 0", "", ""]
    return "\r\n".join(lines).encode()

AFFILIATE_BODY_TPL = (
    '<?xml version="1.0" encoding="UTF-8"?>\r\n'
    '<mcptt-affiliation-command xmlns="urn:3gpp:ns:mcpttAffiliationCommand:1.0">\r\n'
    '  <mcptt-group-aff-control>\r\n'
    '    <mcptt-group-id>{group_uri}</mcptt-group-id>\r\n'
    '    <affiliate/>\r\n'
    '  </mcptt-group-aff-control>\r\n'
    '</mcptt-affiliation-command>\r\n'
)

def make_publish(local_ip: str, local_port: int,
                 user: str, domain: str, group: str, cseq: int,
                 call_id: str, from_tag: str,
                 auth_header: str = "",
                 if_match: str = "") -> bytes:
    group_uri = f"sip:{group}@{domain}"
    body = AFFILIATE_BODY_TPL.format(group_uri=group_uri).encode()
    branch = "z9hG4bK" + uuid.uuid4().hex[:10]
    lines = [
        f"PUBLISH {group_uri} SIP/2.0",
        f"Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport",
        f"From: <sip:{user}@{domain}>;tag={from_tag}",
        f"To: <{group_uri}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} PUBLISH",
        "Max-Forwards: 70",
        "Event: mcptt",
        "Expires: 3600",
        "Content-Type: application/vnd.3gpp.mcptt-affiliation-command+xml",
        "User-Agent: F13-IfMatch-Test/1.0",
    ]
    if auth_header:
        lines.append(auth_header)
    if if_match:
        lines.append(f"SIP-If-Match: {if_match}")
    lines.append(f"Content-Length: {len(body)}")
    lines += ["", ""]
    return "\r\n".join(lines).encode() + body


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

def extract_etag(resp: str) -> str:
    m = re.search(r'^SIP-ETag:\s*(.+)$', resp, re.IGNORECASE | re.MULTILINE)
    return m.group(1).strip() if m else ""


# ── 테스트 ─────────────────────────────────────────────────────────────────

def run(server_ip: str, server_port: int,
        user: str, imsi: str, domain: str, password: str,
        group: str) -> bool:

    username  = f"{imsi}@{domain}"
    reg_uri   = f"sip:{domain}"
    pub_uri   = f"sip:{group}@{domain}"

    print(f"\n{'='*62}")
    print(f"  F-13 SIP-If-Match Test")
    print(f"  CSP    : {server_ip}:{server_port}")
    print(f"  User   : sip:{user}@{domain}")
    print(f"  Group  : {pub_uri}")
    print(f"{'='*62}\n")

    local_port = 51997
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", local_port))

    local_ip  = server_ip
    reg_cid   = f"{uuid.uuid4().hex}@{local_ip}"
    pub_cid   = f"{uuid.uuid4().hex}@{local_ip}"
    from_tag  = uuid.uuid4().hex[:8]
    reg_cseq  = 1
    pub_cseq  = 1
    passed    = True

    try:
        # ── Step 1: REGISTER (no auth) → 401 ─────────────────────────────
        print("  Step 1  REGISTER (인증 없음) → 401 + nonce 수신")
        msg  = make_register(local_ip, local_port, user, domain,
                             reg_cseq, reg_cid, from_tag)
        resp = send_recv(sock, msg, server_ip, server_port)
        if not resp:
            fail("CSP 응답 없음 — IP/Port 확인 필요")
            return False

        status = parse_status(resp)
        nonce  = extract_nonce(resp)
        realm  = extract_realm(resp) or domain
        info(f"응답: {status}  nonce={nonce[:16]}…")
        if status != 401 or not nonce:
            fail(f"기대값 401+nonce, 실제 {status}")
            return False
        ok("401 + nonce 수신")

        # ── Step 2: REGISTER + Digest → 200 OK ───────────────────────────
        reg_cseq += 1
        print("\n  Step 2  REGISTER + Digest → 200 OK (등록 완료)")
        auth = make_auth_header(username, realm, password, nonce, reg_uri,
                                method="REGISTER")
        msg  = make_register(local_ip, local_port, user, domain,
                             reg_cseq, reg_cid, from_tag, auth)
        resp = send_recv(sock, msg, server_ip, server_port)
        status2 = parse_status(resp)
        info(f"응답: {status2}")
        if status2 != 200:
            fail(f"REGISTER 실패 ({status2}) — username/password 확인 필요")
            return False
        ok("200 OK — 등록 완료")

        # ── Step 3: PUBLISH → 200 OK + SIP-ETag ─────────────────────────
        print("\n  Step 3  PUBLISH (최초 affiliate) → 200 OK + SIP-ETag 수령")
        info("(등록된 사용자 PUBLISH — CSP가 별도 Digest 없이 수락)")
        msg  = make_publish(local_ip, local_port, user, domain, group,
                            pub_cseq, pub_cid, from_tag)
        resp = send_recv(sock, msg, server_ip, server_port)
        status3 = parse_status(resp)

        # 401 챌린지가 오면 Digest 붙여 재전송
        if status3 == 401:
            pub_nonce = extract_nonce(resp)
            pub_realm = extract_realm(resp) or domain
            info(f"401 챌린지 수신 — Digest 인증 후 재전송")
            pub_cseq += 1
            auth = make_auth_header(username, pub_realm, password, pub_nonce,
                                    pub_uri, method="PUBLISH")
            msg  = make_publish(local_ip, local_port, user, domain, group,
                                pub_cseq, pub_cid, from_tag, auth)
            resp = send_recv(sock, msg, server_ip, server_port)
            status3 = parse_status(resp)

        etag = extract_etag(resp)
        info(f"응답: {status3}  SIP-ETag={etag}")
        if status3 != 200:
            fail(f"PUBLISH 실패 ({status3})")
            return False
        if not etag:
            fail("SIP-ETag 없음 — F-04 확인 필요")
            return False
        ok(f"200 OK + SIP-ETag 수령: {etag}")

        # ── Step 5: re-PUBLISH + If-Match: 엉뚱한 값 → 412 ──────────────
        pub_cseq += 1
        wrong_etag = "aff-wrong00000000000"
        print(f"\n  Step 5  re-PUBLISH + If-Match: {wrong_etag} → 412 확인")
        info("(앱 충돌 후 잘못된 ETag로 갱신 시도 시뮬)")

        # re-PUBLISH는 새 nonce 필요: 먼저 auth 없이 보내 챌린지 받기
        msg  = make_publish(local_ip, local_port, user, domain, group,
                            pub_cseq, pub_cid, from_tag,
                            if_match=wrong_etag)
        resp = send_recv(sock, msg, server_ip, server_port)
        status5 = parse_status(resp)

        if status5 == 401:
            # 인증 챌린지 → Digest 붙여 재전송
            n5 = extract_nonce(resp)
            r5 = extract_realm(resp) or domain
            auth = make_auth_header(username, r5, password, n5,
                                    pub_uri, method="PUBLISH")
            msg  = make_publish(local_ip, local_port, user, domain, group,
                                pub_cseq, pub_cid, from_tag, auth,
                                if_match=wrong_etag)
            resp = send_recv(sock, msg, server_ip, server_port)
            status5 = parse_status(resp)

        info(f"응답: {status5}")
        if status5 == 412:
            ok("412 Precondition Failed → F-13 PASS (잘못된 ETag 거부)")
        elif status5 == 200:
            fail("200 OK — 잘못된 ETag를 검증 없이 수락 → F-13 FAIL")
            passed = False
        else:
            warn(f"응답: {status5} (예상과 다름)")

        # ── Step 6: re-PUBLISH + If-Match: 올바른 ETag → 200 OK ──────────
        pub_cseq += 1
        print(f"\n  Step 6  re-PUBLISH + If-Match: {etag} → 200 OK 확인")
        info("(올바른 ETag로 정상 갱신)")

        msg  = make_publish(local_ip, local_port, user, domain, group,
                            pub_cseq, pub_cid, from_tag,
                            if_match=etag)
        resp = send_recv(sock, msg, server_ip, server_port)
        status6 = parse_status(resp)

        if status6 == 401:
            n6 = extract_nonce(resp)
            r6 = extract_realm(resp) or domain
            auth = make_auth_header(username, r6, password, n6,
                                    pub_uri, method="PUBLISH")
            msg  = make_publish(local_ip, local_port, user, domain, group,
                                pub_cseq, pub_cid, from_tag, auth,
                                if_match=etag)
            resp = send_recv(sock, msg, server_ip, server_port)
            status6 = parse_status(resp)

        new_etag = extract_etag(resp)
        info(f"응답: {status6}  새 SIP-ETag={new_etag}")
        if status6 == 200:
            ok(f"200 OK — 올바른 ETag로 갱신 성공. 새 ETag: {new_etag}")
        else:
            fail(f"기대값 200, 실제 {status6}")
            passed = False

    finally:
        sock.close()

    print(f"\n{'='*62}")
    if passed:
        print(f"  {GRN}결과: PASS{NC}  F-13 SIP-If-Match 검증 정상 동작")
    else:
        print(f"  {RED}결과: FAIL{NC}  F-13 검증 실패")
    print(f"{'='*62}\n")
    return passed


# ── CLI ───────────────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(description="F-13 SIP-If-Match 검증 테스트")
    p.add_argument("--ip",       default="121.134.202.23",
                   help="CSP IP (기본: 121.134.202.23)")
    p.add_argument("--port",     default=5160, type=int,
                   help="CSP SIP UDP 포트 (기본: 5160)")
    p.add_argument("--user",     default="1001",
                   help="SIP user (기본: 1001)")
    p.add_argument("--imsi",     default="001011000000001",
                   help="Digest username 용 IMSI (기본: 001011000000001)")
    p.add_argument("--domain",   default="csp",
                   help="SIP domain / realm (기본: csp)")
    p.add_argument("--password", default="1234",
                   help="비밀번호 (기본: 1234)")
    p.add_argument("--group",    default="g001",
                   help="테스트 그룹 ID (기본: g001)")
    args = p.parse_args()

    ok_flag = run(args.ip, args.port,
                  args.user, args.imsi, args.domain, args.password,
                  args.group)
    sys.exit(0 if ok_flag else 1)


if __name__ == "__main__":
    main()
