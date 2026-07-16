"""CMP UDP 클라이언트 — 9000 포트, JSON-over-UDP (envelope v2).

wire 규격은 docs/api/cmp_media_api.md 가 정본.
fire-and-forget 또는 응답 폴링 모두 지원.
"""
from __future__ import annotations

import json
import socket
import time


def cmp_request(payload: dict, ip: str = "127.0.0.1", port: int = 9000,
                timeout: float = 1.0) -> dict | None:
    """envelope v2 로 전송, 응답을 dict 로 반환 (None=timeout).

    편의상 payload dict 안의 'cmd'/'sesid'/'service' 키는 hdr 로 승격되고,
    나머지 키만 wire payload 로 나간다. 응답은 {'hdr': {...}, 'payload': {...}}.
    """
    p = dict(payload)
    hdr = {
        "ver": 2,
        "trans_id": int(time.time() * 1000) % 1000000,
        "node": "verify",
        "cmd": p.pop("cmd", ""),
        "type": "request",
    }
    sesid = p.pop("sesid", None)
    service = p.pop("service", None)
    if sesid:
        hdr["sesid"] = sesid
    if service:
        hdr["service"] = service
    msg_obj: dict = {"hdr": hdr}
    if p:
        msg_obj["payload"] = p
    msg = json.dumps(msg_obj).encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(msg, (ip, port))
        try:
            data, _ = s.recvfrom(4096)
            return json.loads(data.decode())
        except socket.timeout:
            return None
    finally:
        s.close()


def cmp_ok(resp: dict | None) -> bool:
    """응답 hdr.status == OK 판정."""
    return bool(resp) and (resp.get("hdr") or {}).get("status") == "OK"


def cmp_stats(ip: str = "127.0.0.1", port: int = 9000,
              timeout: float = 1.0) -> dict | None:
    """STATS 조회 → flat dict (None=무응답/실패).

    v2 payload {resource, detail} 를 verify 아이템들이 쓰기 쉬운 flat 키
    (sessions/groups/rtp_ports_*/group_details)로 정규화해 반환한다.
    """
    resp = cmp_request({"cmd": "STATS"}, ip=ip, port=port, timeout=timeout)
    if not cmp_ok(resp):
        return None
    p = (resp or {}).get("payload") or {}
    res = p.get("resource") or {}
    relay = res.get("relay") or {}
    ptt = res.get("ptt") or {}
    det = p.get("detail") or {}
    return {
        "sessions": relay.get("sessions", 0),
        "groups": ptt.get("groups", 0),
        "rtp_ports_total": relay.get("total", 0),
        "rtp_ports_used": relay.get("used", 0),
        "ptt_rtp_ports_total": ptt.get("total", 0),
        "ptt_rtp_ports_used": ptt.get("used", 0),
        "joined": ptt.get("joined", 0),
        "group_details": det.get("groups", []) or [],
    }


def remove_group(group_id: str, ip: str = "127.0.0.1", port: int = 9000) -> None:
    """CMP 의 그룹 세션 제거 (fire-and-forget, 응답 무시).

    ⚠️ CSP 캐시 (m_mapPttSession / CGroupMap) 는 동기화되지 않으므로 호출 후
    CSP 가 다음 INVITE 처리 시 PTT_GROUP_ADD 를 다시 보내지 않고 곧장
    PTT_JOIN 으로 가서 'group not found' 가 발생할 수 있다. CSP 의
    cache 도 정리하려면 cims.sh reset --all (prep-reset preset) 로 CSP 재시작
    경로를 사용할 것.
    """
    if not group_id:
        return
    try:
        cmp_request({"cmd": "PTT_GROUP_REMOVE", "group_id": group_id}, ip, port,
                    timeout=1.0)
    except Exception:
        pass
