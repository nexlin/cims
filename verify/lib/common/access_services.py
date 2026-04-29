"""access_services.jsonl 시드 + CSP SIGUSR1 reload helper.

Phase 1/3 회귀 시 cspsim 가 사용할 service domain 매핑을 jsonl 로 작성.
"""
from __future__ import annotations

import json
import os
import time
import uuid

from .subscribers import VOLTE_DOMAIN, MCPTT_DOMAIN


def seed_access_services(cfg_dir: str, voip_ref: str, ptt_ref: str,
                          tag: str = "verify-seed",
                          note: str = "auto-seeded by verify.lib") -> int:
    """{cfg_dir}/access_services.jsonl 작성. 작성 건수 반환."""
    seeded = []

    def add(name: str, kind: str, domain: str) -> None:
        if not name:
            return
        seeded.append({
            "id": uuid.uuid4().hex, "name": name, "enabled": True,
            "kind": kind, "domain": domain, "auth_realm": domain,
            "inbound_policy": "any", "allowed_local_node_refs": [],
            "priority": 100, "tags": [tag], "note": note,
            "server_identity_uri": f"sip:cspserver@{domain}",
        })

    add(voip_ref, "volte", VOLTE_DOMAIN)
    add(ptt_ref,  "ptt",   MCPTT_DOMAIN)
    if not seeded:
        return 0
    os.makedirs(cfg_dir, exist_ok=True)
    path = os.path.join(cfg_dir, "access_services.jsonl")
    with open(path, "w") as f:
        for r in seeded:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return len(seeded)


def signal_csp_reload(pid_file: str, wait_sec: float = 2.0) -> bool:
    """{pid_file} 의 PID 에 SIGUSR1(10) 전송하여 jsonl 재로드 트리거."""
    try:
        with open(pid_file) as pf:
            pid = int(pf.read().strip())
        os.kill(pid, 10)
        time.sleep(wait_sec)
        return True
    except Exception:
        return False
