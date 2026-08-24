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
import os
import re
import socket
import ssl
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
                    auth_header: str = "", transport: str = "UDP",
                    expires: int = 60, extra: list | None = None) -> str:
    aor = f"sip:{user}@{domain}"
    tr = transport.upper()
    contact_tr = f";transport={tr.lower()}" if tr != "UDP" else ""
    lines = [
        f"REGISTER sip:{domain} SIP/2.0",
        f"Via: SIP/2.0/{tr} {local_ip}:{local_port};branch={branch};rport",
        "Max-Forwards: 70",
        f"From: <{aor}>;tag={from_tag}",
        f"To: <{aor}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} REGISTER",
        f"Contact: <sip:{user}@{local_ip}:{local_port}{contact_tr}>",
        f"Expires: {expires}",
    ]
    lines += list(extra or [])
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


# ── RFC 3329 sec-agree (sip_access_security.md §8.1, P2) — TLS 위 원시 프로브 ─────────────────

_HEADER_RE = re.compile(rb"^([A-Za-z\-]+):\s*(.*?)\r?$", re.MULTILINE)


def _recv_final_stream(sock, deadline: float) -> tuple:
    """스트림(TLS) 소켓에서 첫 최종 응답 1건 — 헤더 종료(CRLFCRLF)+Content-Length 만큼 읽는다."""
    buf = b""
    while time.time() < deadline:
        sock.settimeout(max(deadline - time.time(), 0.1))
        try:
            chunk = sock.recv(65535)
        except (socket.timeout, ssl.SSLError, OSError):
            break
        if not chunk:
            break
        buf += chunk
        while True:
            end = buf.find(b"\r\n\r\n")
            if end < 0:
                break
            head = buf[:end + 4]
            m = re.search(rb"Content-Length:\s*(\d+)", head, re.IGNORECASE)
            clen = int(m.group(1)) if m else 0
            if len(buf) < end + 4 + clen:
                break
            msg, buf = buf[:end + 4 + clen], buf[end + 4 + clen:]
            sm = _STATUS_RE.search(msg)
            if not sm:
                continue
            code = int(sm.group(1))
            if code < 200:
                continue
            return code, msg
    return 0, b""


def _header(raw: bytes, name: str) -> str:
    m = re.search((r"^" + re.escape(name) + r":\s*(.*?)\r?$").encode(), raw,
                  re.IGNORECASE | re.MULTILINE)
    return m.group(1).decode().strip() if m else ""


class SecAgreeTlsSession:
    """TLS 연결 하나를 열어 두고 sec-agree 등록 절차를 단계별로 수행한다.

    `register(...)` 가 (1차 코드, 2차 코드, 1차 Security-Server, 2차 raw) 를 돌려주고, 연결은
    close() 까지 유지된다 — 등록 바인딩이 살아있는 동안 다른 프로브(UDP MESSAGE → 403 게이트)를
    끼워 넣기 위해서다. 서버 인증서는 검증하지 않는다(사설 CA/자가서명 dev).
    """

    def __init__(self, server_ip: str, tls_port: int, local_ip: str, timeout: float = 4.0):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.bind((local_ip, 0))
        raw.settimeout(timeout)
        raw.connect((server_ip, tls_port))
        self.sock = ctx.wrap_socket(raw, server_hostname=server_ip)
        self.local_ip, self.local_port = self.sock.getsockname()[:2]
        self.timeout = timeout
        self.server_realm = ""

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def _send(self, msg: str) -> None:
        self.sock.sendall(msg.replace("\n", "\r\n").encode())

    def register(self, user: str, domain: str, auth_user: str, ha1_hex: str, password: str,
                 security_client: str | None = "tls", require: bool = True,
                 verify: str | None = "echo", expires: int = 60) -> dict:
        """sec-agree 등록 절차.

        security_client: 초기/재 REGISTER 의 Security-Client 값 (None = 헤더 생략).
        require        : Require/Proxy-Require: sec-agree 동봉 여부.
        verify         : 재-REGISTER 의 Security-Verify — "echo" = 1차 응답 Security-Server 그대로,
                         None = 생략, 그 외 문자열 = 그 값(변조 재현).
        반환: {"first", "second", "security_server", "second_security_server", "service_route"}.
        """
        sa = []
        if security_client is not None:
            sa.append(f"Security-Client: {security_client}")
        if require:
            sa += ["Require: sec-agree", "Proxy-Require: sec-agree"]
        call_id, from_tag = _callid(self.local_ip), _tag()
        msg1 = _register_lines(user, domain, self.local_ip, self.local_port, call_id, from_tag, 1,
                               _branch(), transport="TLS", expires=expires, extra=sa)
        self._send(msg1)
        code1, raw1 = _recv_final_stream(self.sock, time.time() + self.timeout)
        out = {"first": code1, "second": 0, "security_server": _header(raw1, "Security-Server"),
               "second_security_server": "", "service_route": ""}
        if code1 != 401:
            return out
        ch = _parse_challenge(raw1)
        self.server_realm = ch.get("realm", "")
        nonce = ch.get("nonce", "")
        uri = f"sip:{domain}"
        resp = _digest_response(auth_user, self.server_realm, password, ha1_hex, "REGISTER", uri, nonce)
        auth = (f'Authorization: Digest username="{auth_user}", realm="{self.server_realm}", '
                f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=MD5')
        sa2 = list(sa)
        if verify == "echo":
            if out["security_server"]:
                sa2.append(f"Security-Verify: {out['security_server']}")
        elif verify is not None:
            sa2.append(f"Security-Verify: {verify}")
        msg2 = _register_lines(user, domain, self.local_ip, self.local_port, call_id, from_tag, 2,
                               _branch(), auth_header=auth, transport="TLS", expires=expires, extra=sa2)
        self._send(msg2)
        code2, raw2 = _recv_final_stream(self.sock, time.time() + self.timeout)
        out["second"] = code2
        out["second_security_server"] = _header(raw2, "Security-Server")
        out["service_route"] = _header(raw2, "Service-Route")
        return out


# ── IMS AKA over TLS (sip_access_security.md §8.2, P3) — 소프트-USIM 원시 프로브 ─────────────────

def _csc_auc_modules():
    """csc 의 Milenage 구현을 소프트-USIM 으로 빌려 쓴다 (csc/src/services/auc). 단말 계산의 독립 참조가
    아니라 같은 코드지만, 서버(CSP)가 검증하는 XRES 는 CSC 가 그 코드로 만들고 단말 답안은 여기서 만들므로
    CSP↔CSC↔단말 세 변의 정합(nonce 조립·H(A1)=MD5(impi:realm:RES 이진)·AUTS)이 검증된다.
    cspsim(psip SipAka.cpp — OpenSSL) 경로는 별도 C++ 하네스(tests/psip_aka_test.cpp)가 같은 벡터로 본다."""
    import importlib
    import sys
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    p = os.path.join(repo, "csc", "src")
    if p not in sys.path:
        sys.path.insert(0, p)
    return importlib.import_module("services.auc.milenage")


def aka_answer(k_hex: str, opc_hex: str, nonce_b64: str, sqn_ms: int) -> dict:
    """챌린지 nonce=base64(RAND‖AUTN) 에 대한 단말 계산.

    반환: {"mac_ok", "sqn_ok", "sqn", "res"(bytes), "auts_b64"(sqn 이탈 시), "rand"(bytes)}.
    """
    import base64
    m = _csc_auc_modules()
    raw = base64.b64decode(nonce_b64)
    rand, autn = raw[:16], raw[16:32]
    k, opc = bytes.fromhex(k_hex), bytes.fromhex(opc_hex)
    mil = m.Milenage(k, opc)
    res, ak = mil.f2_f5(rand)
    sqn = bytes(a ^ b for a, b in zip(autn[:6], ak))
    amf, mac = autn[6:8], autn[8:]
    mac_a, _ = mil.f1_f1star(rand, sqn, amf)
    out = {"mac_ok": mac_a == mac, "sqn_ok": False, "sqn": int.from_bytes(sqn, "big"),
           "res": res, "auts_b64": "", "rand": rand}
    if not out["mac_ok"]:
        return out
    out["sqn_ok"] = out["sqn"] > sqn_ms
    if not out["sqn_ok"]:
        sqn_ms_b = m.sqn_to_bytes(sqn_ms)
        _, mac_s = mil.f1_f1star(rand, sqn_ms_b, b"\x00\x00")
        auts = bytes(a ^ b for a, b in zip(sqn_ms_b, mil.f5star(rand))) + mac_s
        out["auts_b64"] = base64.b64encode(auts).decode()
    return out


def aka_auts_hex(k_hex: str, opc_hex: str, rand: bytes, sqn_ms: int) -> str:
    """서버 내부 AV API(재동기) 시험용 — 단말이 만들 AUTS 를 hex 로."""
    m = _csc_auc_modules()
    mil = m.Milenage(bytes.fromhex(k_hex), bytes.fromhex(opc_hex))
    sqn_ms_b = m.sqn_to_bytes(sqn_ms)
    _, mac_s = mil.f1_f1star(rand, sqn_ms_b, b"\x00\x00")
    return (bytes(a ^ b for a, b in zip(sqn_ms_b, mil.f5star(rand))) + mac_s).hex()


def _akav1_response(user: str, realm: str, res: bytes, method: str, uri: str,
                    nonce: str, cnonce: str = "1", nc: str = "00000001") -> str:
    """RFC 3310 AKAv1-MD5 + qop=auth: H(A1)=MD5(user:realm:RES 이진)."""
    ha1 = hashlib.md5(f"{user}:{realm}:".encode() + res).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    return hashlib.md5(f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}".encode()).hexdigest()


class AkaTlsSession(SecAgreeTlsSession):
    """TLS 위 IMS AKA 등록/해제 (AKAv1-MD5, RFC 3310 / TS 24.229 §5.1.1.5.1)."""

    def _aka_flow(self, user: str, domain: str, auth_user: str, k_hex: str, opc_hex: str,
                  sqn_ms: int, expires: int) -> dict:
        call_id, from_tag = _callid(self.local_ip), _tag()
        msg1 = _register_lines(user, domain, self.local_ip, self.local_port, call_id, from_tag, 1,
                               _branch(), transport="TLS", expires=expires)
        self._send(msg1)
        code1, raw1 = _recv_final_stream(self.sock, time.time() + self.timeout)
        ch = _parse_challenge(raw1)
        out = {"first": code1, "algorithm": ch.get("algorithm", ""), "second": 0, "resync": False,
               "third": 0, "service_route": "", "sqn": 0}
        if code1 != 401:
            return out
        self.server_realm = ch.get("realm", "")
        uri = f"sip:{domain}"

        def answer(nonce: str, cseq_n: int) -> tuple:
            ans = aka_answer(k_hex, opc_hex, nonce, sqn_ms)
            params = ""
            if not ans["mac_ok"]:
                resp = ""                                   # MAC 실패 보고 — 빈 response, auts 없음
            elif not ans["sqn_ok"]:
                resp = _akav1_response(auth_user, self.server_realm, b"", "REGISTER", uri, nonce)
                params = f', auts="{ans["auts_b64"]}"'      # 빈 password 로 계산 + auts
            else:
                resp = _akav1_response(auth_user, self.server_realm, ans["res"], "REGISTER", uri, nonce)
            auth = (f'Authorization: Digest username="{auth_user}", realm="{self.server_realm}", '
                    f'nonce="{nonce}", uri="{uri}", response="{resp}", algorithm=AKAv1-MD5, '
                    f'cnonce="1", nc=00000001, qop=auth{params}')
            msg = _register_lines(user, domain, self.local_ip, self.local_port, call_id, from_tag, cseq_n,
                                  _branch(), auth_header=auth, transport="TLS", expires=expires)
            self._send(msg)
            code, raw = _recv_final_stream(self.sock, time.time() + self.timeout)
            return ans, code, raw

        ans, code2, raw2 = answer(ch.get("nonce", ""), 2)
        out["second"] = code2
        out["sqn"] = ans["sqn"]
        out["service_route"] = _header(raw2, "Service-Route")
        if ans["mac_ok"] and not ans["sqn_ok"] and code2 == 401:
            # 재동기 후 새 챌린지 — 서버 SQN 은 SQN_MS+1 이라 이제 신선하다
            out["resync"] = True
            ch3 = _parse_challenge(raw2)
            ans3, code3, raw3 = answer(ch3.get("nonce", ""), 3)
            out["third"] = code3
            out["sqn"] = ans3["sqn"]
            out["service_route"] = _header(raw3, "Service-Route")
        return out

    def register_aka(self, user: str, domain: str, auth_user: str, k_hex: str, opc_hex: str,
                     sqn_ms: int = 0, expires: int = 60) -> dict:
        """1차 REGISTER(무인증) → 401(AKA 챌린지) → 답안 REGISTER → 최종 코드.

        sqn_ms 가 챌린지 SQN 이상이면 단말은 auts 를 보내고(재동기), 서버의 두 번째 401 에 다시 답한다.
        k_hex 가 서버 K 와 다르면 AUTN MAC 이 틀려 빈 response 를 보낸다(서버 403 기대).
        반환: {"first","algorithm","second","resync","third","service_route","sqn"}.
        """
        return self._aka_flow(user, domain, auth_user, k_hex, opc_hex, sqn_ms, expires)

    def unregister_aka(self, user: str, domain: str, auth_user: str, k_hex: str, opc_hex: str,
                       sqn_ms: int = 0) -> int:
        """Expires: 0 등록 해제 — 해제도 AKA 답안이 필요하다. 최종 코드."""
        r = self._aka_flow(user, domain, auth_user, k_hex, opc_hex, sqn_ms, 0)
        return r["third"] if r["resync"] else r["second"]
