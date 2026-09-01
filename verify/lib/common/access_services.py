"""access_services.jsonl 시드 + CSP SIGUSR1 reload helper.

Phase 1/3 회귀 시 cspsim 가 사용할 service domain 매핑을 jsonl 로 작성.
"""
from __future__ import annotations

import json
import os
import time
import uuid

from .subscribers import VOLTE_DOMAIN, MCPTT_DOMAIN


# 호 전달 금지 정책 서비스 — S3-SCN-XFER 가 전달자 service_ref 를 잠시 이 서비스로 돌려
#   REFER 403 게이트(volte_supplementary_services.md §6.3)를 본다. voip 서비스와 같은 도메인·realm 이라
#   등록/인증은 동일하고, priority 가 낮아(=값이 커) GetByKind/도메인 매핑의 1순위를 빼앗지 않는다.
NOXFER_SERVICE_REF = "volte-noxfer"


def seed_access_services(cfg_dir: str, voip_ref: str, ptt_ref: str,
                          tag: str = "verify-seed",
                          note: str = "auto-seeded by verify.lib",
                          with_noxfer: bool = False) -> int:
    """{cfg_dir}/access_services.jsonl 작성. 작성 건수 반환.

    volte 레코드는 관제 보조 서비스 필드(`pickup_feature_code`="**", `transfer_allowed`=true)를 명시해
    서비스별 피처코드 경로(전역 CallPickupId 폴백 아님)를 태운다. with_noxfer 면 `transfer_allowed=false`
    변종(NOXFER_SERVICE_REF)을 하나 더 쓴다.
    """
    seeded = []

    def add(name: str, kind: str, domain: str, priority: int = 100, **extra) -> None:
        if not name:
            return
        rec = {
            "id": uuid.uuid4().hex, "name": name, "enabled": True,
            "kind": kind, "domain": domain, "auth_realm": domain,
            "inbound_policy": "any", "allowed_local_node_refs": [],
            "priority": priority, "tags": [tag], "note": note,
            "server_identity_uri": f"sip:cspserver@{domain}",
        }
        if kind == "volte":
            rec.update({"pickup_feature_code": "**", "transfer_allowed": True})
        rec.update(extra)
        seeded.append(rec)

    add(voip_ref, "volte", VOLTE_DOMAIN)
    add(ptt_ref,  "ptt",   MCPTT_DOMAIN)
    if with_noxfer and voip_ref:
        add(NOXFER_SERVICE_REF, "volte", VOLTE_DOMAIN, priority=200, transfer_allowed=False)
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
