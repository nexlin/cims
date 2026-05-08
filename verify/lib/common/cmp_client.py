"""CMP UDP 클라이언트 — 9000 포트, JSON-over-UDP.

fire-and-forget 또는 응답 폴링 모두 지원.
"""
from __future__ import annotations

import json
import socket
import time


def cmp_request(payload: dict, ip: str = "127.0.0.1", port: int = 9000,
                timeout: float = 1.0) -> dict | None:
    """{trans_id, payload} 래핑하여 전송, 응답을 dict 로 반환 (None=timeout)."""
    msg = json.dumps({
        "trans_id": int(time.time() * 1000) % 1000000,
        "payload": payload,
    }).encode()
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


def remove_group(group_id: str, ip: str = "127.0.0.1", port: int = 9000) -> None:
    """CMP 의 그룹 세션 제거 (fire-and-forget, 응답 무시).

    ⚠️ CSP 캐시 (m_mapPttSession / CGroupMap) 는 동기화되지 않으므로 호출 후
    CSP 가 다음 INVITE 처리 시 ADD_PTT_GROUP 을 다시 보내지 않고 곧장
    JOIN_PTT_GROUP 으로 가서 'Group Not Found' 가 발생할 수 있다. CSP 의
    cache 도 정리하려면 cims.sh reset --all (prep-reset preset) 로 CSP 재시작
    경로를 사용할 것.

    cmd 는 CSP↔CMP 프로토콜 규약대로 대문자 'REMOVE_PTT_GROUP'.
    """
    if not group_id:
        return
    try:
        cmp_request({"cmd": "REMOVE_PTT_GROUP", "group_id": group_id}, ip, port,
                    timeout=1.0)
    except Exception:
        pass
