"""원시 SIP 프로브 — 채널 정책 게이트·realm 대조 검증용 (sip_access_security.md §6).

cspsim 은 call 시나리오에서 `-transport` 를 무시하고 INVITE 를 UDP 로만 보내므로
(§7 잔여), 게이트(§3)와 realm 대조(§4.6)의 "인증보다 먼저" 판정은 cspsim 통계로는
재현되지 않는다. 이 helper 는 최소 REGISTER/INVITE 를 UDP 로 직접 조립해 **첫 최종
응답 코드**를 돌려준다 — 게이트가 401(챌린지)보다 먼저 403 을 주는지, realm 불일치가
401 재챌린지가 되는지를 결정적으로 판정한다.

의존: 표준 라이브러리만 (socket/hashlib). CSP 는 자가서명 TLS 라 UDP 평문 경로만 쓴다.
"""
from __future__ import annotations

import hashlib
import re
import socket
import time
import uuid

_STATUS_RE = re.compile(rb"^SIP/2\.0\s+(\d{3})\s", re.MULTILINE)


def _branch() -> str:
    return "z9hG4bK" + uuid.uuid4().hex[:16]


def _tag() -> str:
    return uuid.uuid4().hex[:12]


def _callid(local_ip: str) -> str:
    return f"{uuid.uuid4().hex}@{local_ip}"


def _recv_final(sock: socket.socket, deadline: float) -> tuple:
    """첫 **최종**(>=200) 응답의 (status:int, raw:bytes) 반환. 1xx 는 건너뛴다.

    타임아웃/무응답이면 (0, b'') — 게이트가 조용히 폐기(무응답)한 경우도 이 값.
    """
    while time.time() < deadline:
        sock.settimeout(max(deadline - time.time(), 0.1))
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            break
        except OSError:
            break
        m = _STATUS_RE.search(data)
        if not m:
            continue
        code = int(m.group(1))
        if code < 200:
            continue  # 1xx provisional (INVITE 100 Trying) — 최종 대기
        return code, data
    return 0, b""


def _send(sock: socket.socket, server: tuple, msg: str) -> None:
    sock.sendto(msg.replace("\n", "\r\n").encode(), server)


def _register_lines(user: str, domain: str, local_ip: str, local_port: int,
                    call_id: str, from_tag: str, cseq: int, branch: str,
                    auth_header: str = "") -> str:
    aor = f"sip:{user}@{domain}"
    lines = [
        f"REGISTER sip:{domain} SIP/2.0",
        f"Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport",
        "Max-Forwards: 70",
        f"From: <{aor}>;tag={from_tag}",
        f"To: <{aor}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} REGISTER",
        f"Contact: <sip:{user}@{local_ip}:{local_port}>",
        "Expires: 60",
    ]
    if auth_header:
        lines.append(auth_header)
    lines += ["Content-Length: 0", "", ""]
    return "\n".join(lines)


def probe_register(server_ip: str, server_port: int, user: str, domain: str,
                   local_ip: str, local_port: int = 0, timeout: float = 3.0) -> int:
    """인증 없는 REGISTER 1건 → 첫 최종 응답 코드.

    정상 가입자: 401(챌린지). TLS 강제 가입자의 UDP 요청: 403(게이트 — 챌린지 앞).
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((local_ip, local_port))
        lp = sock.getsockname()[1]
        msg = _register_lines(user, domain, local_ip, lp, _callid(local_ip),
                              _tag(), 1, _branch())
        _send(sock, (server_ip, server_port), msg)
        code, _ = _recv_final(sock, time.time() + timeout)
        return code
    finally:
        sock.close()


def probe_nonregister(server_ip: str, server_port: int, user: str, domain: str,
                      local_ip: str, method: str = "MESSAGE", local_port: int = 0,
                      timeout: float = 3.0) -> int:
    """인증 없는 비-REGISTER 요청 1건 → 첫 최종 응답 코드 (1xx 제외).

    "게이트가 인증보다 먼저"(§3.2)의 검증축. 정상 가입자: 401(챌린지). TLS 강제
    가입자의 UDP 요청: 403(게이트 — 인증 앞).

    method 기본은 MESSAGE 다. INVITE 를 쓰지 않는 이유:
      · dev 의 `Setup.TestEnvOpenTermination` 은 착신이 로컬 가입자/그룹인 INVITE 를
        게이트 앞에서 통과시킨다(수신통화 허용, ModuleDispatcher.cpp §515) — 상용은
        flag off 라 게이트가 걸리지만 dev 에서는 이 단락으로 게이트에 도달하지 않는다.
      · SDP 없는 INVITE 는 미디어 협상 단계에서 488 로 조기 거절되어 판정이 흐려진다.
    MESSAGE 는 두 우회를 모두 피해 게이트를 직접 탄다(To 무관 403). OPTIONS 는 스택이
    dispatcher 앞에서 200 으로 답해 게이트 밖이므로 쓰지 않는다.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((local_ip, local_port))
        lp = sock.getsockname()[1]
        aor = f"sip:{user}@{domain}"
        msg = "\n".join([
            f"{method} {aor} SIP/2.0",
            f"Via: SIP/2.0/UDP {local_ip}:{lp};branch={_branch()};rport",
            "Max-Forwards: 70",
            f"From: <{aor}>;tag={_tag()}",
            f"To: <{aor}>",
            f"Call-ID: {_callid(local_ip)}",
            f"CSeq: 1 {method}",
            f"Contact: <sip:{user}@{local_ip}:{lp}>",
            "Content-Length: 0", "", "",
        ])
        _send(sock, (server_ip, server_port), msg)
        code, _ = _recv_final(sock, time.time() + timeout)
        return code
    finally:
        sock.close()


def _parse_challenge(raw: bytes) -> dict:
    """401 응답의 WWW-Authenticate 에서 realm/nonce/qop 추출."""
    out: dict = {}
    m = re.search(rb"WWW-Authenticate:\s*Digest\s+(.+)", raw, re.IGNORECASE)
    if not m:
        return out
    for key in ("realm", "nonce", "qop", "algorithm"):
        km = re.search((key + r'="?([^",\r\n]+)"?').encode(), m.group(1))
        if km:
            out[key] = km.group(1).decode()
    return out


def _digest_response(user: str, realm: str, password: str, ha1_hex: str,
                     method: str, uri: str, nonce: str) -> str:
    """qop 없는 RFC 2617 response. ha1_hex 우선, 없으면 password 로 A1 계산."""
    if ha1_hex:
        ha1 = ha1_hex
    else:
        ha1 = hashlib.md5(f"{user}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    return hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()


def probe_register_wrong_realm(server_ip: str, server_port: int, user: str,
                               domain: str, auth_user: str, ha1_hex: str,
                               password: str, local_ip: str,
                               wrong_realm: str = "wrong.realm.invalid",
                               local_port: int = 0, timeout: float = 3.0) -> dict:
    """realm 대조(§4.6 P1-a) 프로브.

    1) 인증 없는 REGISTER → 401 챌린지(서버 realm/nonce 취득).
    2) **틀린 realm** 으로 Authorization 을 만들어 재전송 → 서버는 realm 불일치를
       response 검증 전에 잡아 401 재챌린지해야 한다(200 이 아니다).

    반환: {"first": <1차 코드>, "second": <2차 코드>, "server_realm": <str>}.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((local_ip, local_port))
        lp = sock.getsockname()[1]
        call_id, from_tag = _callid(local_ip), _tag()
        msg1 = _register_lines(user, domain, local_ip, lp, call_id, from_tag, 1, _branch())
        _send(sock, (server_ip, server_port), msg1)
        code1, raw1 = _recv_final(sock, time.time() + timeout)
        out = {"first": code1, "second": 0, "server_realm": ""}
        if code1 != 401:
            return out
        ch = _parse_challenge(raw1)
        out["server_realm"] = ch.get("realm", "")
        nonce = ch.get("nonce", "")
        uri = f"sip:{domain}"
        resp = _digest_response(auth_user, wrong_realm, password, ha1_hex,
                                "REGISTER", uri, nonce)
        auth = (f'Authorization: Digest username="{auth_user}", realm="{wrong_realm}", '
                f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=MD5')
        msg2 = _register_lines(user, domain, local_ip, lp, call_id, from_tag, 2,
                              _branch(), auth_header=auth)
        _send(sock, (server_ip, server_port), msg2)
        code2, _ = _recv_final(sock, time.time() + timeout)
        out["second"] = code2
        return out
    finally:
        sock.close()
