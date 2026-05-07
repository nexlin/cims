"""CSP UDP notify helper — csc/services/mcptt.py::notify_csp 와 동일 패턴.

CSP 의 `CCscInterface` 가 4421/UDP 에서 JSON 이벤트를 수신한다 (GROUP_CHANGED /
USER_CHANGED / LISTENER_CHANGED 등). 평소엔 csc admin 이 발송하지만 verify
단계에서 csc 를 거치지 않고 csp 캐시를 직접 강제 동기화해야 할 때 사용.

대표 케이스: S6-SEED 끝에서 csp 의 OnGroupConfigChanged() 강제 트리거 →
SyncGroupsState (CMP addGroup) + CheckGroupIntegrity (멤버 invite). pipeline
회차에서 csp fresh start 후 첫 SyncGroupsState 까지 ~89s 걸리는 wait 우회.
"""
from __future__ import annotations

import json
import socket
import time


CSP_NOTIFY_IP = "127.0.0.1"
CSP_NOTIFY_PORT = 4421


def notify_csp_event(event_type: str, uri: str = "", action: str = "POST",
                     *, ip: str = CSP_NOTIFY_IP, port: int = CSP_NOTIFY_PORT,
                     timeout: float = 0.5) -> bool:
    """CSP 4421/UDP 에 notify event 1건 발송 (fire-and-forget).

    return: send 성공 시 True. socket exception 시 False.
    """
    payload = {
        "trans_id": str(int(time.time() * 1000)),
        "event": event_type,
        "uri": uri,
        "action": action,
        "etag": "",
        "sesid": "",
        "service": "console",
    }
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(json.dumps(payload).encode(), (ip, port))
        return True
    except Exception:
        return False
    finally:
        s.close()


def trigger_group_resync(group_uri: str = "tel:verify-resync",
                         *, ip: str = CSP_NOTIFY_IP,
                         port: int = CSP_NOTIFY_PORT) -> bool:
    """GROUP_CHANGED 1회 발송 → CSP `OnGroupConfigChanged()` 강제 호출.

    `OnGroupConfigChanged` 는 URI 무관하게 LoadFromDb + SyncGroupsState +
    CheckMemberState + CheckGroupIntegrity 를 실행 — 즉 dummy URI 라도 결과는
    전체 그룹 재동기화. CMP addGroup + 등록 멤버 자동 invite 까지 트리거.
    """
    return notify_csp_event("GROUP_CHANGED", group_uri, "PUT", ip=ip, port=port)
