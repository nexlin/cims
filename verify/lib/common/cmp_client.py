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
    """그룹 세션 제거 (fire-and-forget, 응답 무시)."""
    if not group_id:
        return
    try:
        cmp_request({"cmd": "removeGroup", "group_id": group_id}, ip, port,
                    timeout=1.0)
    except Exception:
        pass
