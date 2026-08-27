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


def seed_tls_local_node(dist_dir: str, bind_ip: str, port: int = 5061,
                        node_id: str = "verify-stage3-tls") -> str:
    """{dist_dir}/config/local_nodes.jsonl 에 dev TLS 접속점을 시드한다 — 라이브 토폴로지
    (UDP/TCP/TLS 접속점 공존)를 dev 스택에 반영해 TLS 전제 시나리오(sec-agree·AKA over TLS·
    TLS 재바인드)가 임시 리스너 없이 돈다. 인증서는 dist csc 자가서명(프로브는 서버 인증서를
    검증하지 않는다). 반환: 'added' | 'exists' | 'skip:no-cert' | 'skip:no-file'."""
    path = os.path.join(dist_dir, "config", "local_nodes.jsonl")
    if not os.path.isfile(path):
        return "skip:no-file"
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            try:
                r = json.loads(line)
            except Exception:
                continue
            if str(r.get("protocol", "")).upper() == "TLS" and r.get("enabled", True):
                return "exists"
    cert = os.path.join(dist_dir, "csc", "cert", "server.crt")
    key = os.path.join(dist_dir, "csc", "cert", "server.key")
    if not (os.path.isfile(cert) and os.path.isfile(key)):
        return "skip:no-cert"
    rec = {"id": node_id, "name": node_id, "edge": "access",
           "bind_ip": bind_ip, "bind_port": port, "protocol": "TLS",
           "enabled": True, "is_primary": False,
           "tls_cert_path": cert, "tls_key_path": key, "tls_verify_peer": False}
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    return "added"


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
