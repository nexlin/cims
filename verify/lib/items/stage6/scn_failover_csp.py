"""S6-SCN-FAILOVER-CSP — CSP active kill → 단말 REGISTER 복원.

Phase 1.H stub. 2-node 환경 + Redis 활성 (`csp.json` Redis.Host 설정 또는
ha.json 존재) 일 때만 LIVE. 현재 dev 환경 SKIP.

LIVE 흐름:
  1. ha.json + Redis 활성 확인.
  2. 사전 단말 1개 REGISTER → Redis SET 확인.
  3. active CSP kill.
  4. ≤5s 대기 후 peer CSP 가 VIP 인수 + 단말 신규 REGISTER 시 Redis lookup hit
     → CspUserMap 에 복원 + 200 OK 응답.
"""
from __future__ import annotations

import json
import os
import socket

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_HA_JSON_REL = ("agent", "keepalived", "ha.json")


@verify_item(
    id="S6-SCN-FAILOVER-CSP", stage=6, category="시나리오",
    name="HA fail-over — CSP active kill → 단말 REGISTER 복원",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full"],
    side_effects=["process-state", "network"], timeout_s=30,
    execution_order=96,
)
def scn_failover_csp(ctx: VerifyContext) -> ItemResult:
    ha_path = os.path.join(ctx.dist_dir, *_HA_JSON_REL)
    if not os.path.isfile(ha_path):
        return _skip(ctx, f"ha.json 없음 ({ha_path}) — HA 비활성 환경, SKIP")
    try:
        cfg = json.load(open(ha_path))
    except Exception as e:
        return _skip(ctx, f"ha.json 파싱 실패: {e}")
    peer_ip = cfg.get("peer_ip") or ""
    if not peer_ip:
        return _skip(ctx, "ha.json.peer_ip 미설정")
    if not _peer_sip_ready(peer_ip, 5060):
        return _skip(ctx, f"peer {peer_ip}:5060/UDP bind 미감지 — 2-node 미준비")

    # Redis 활성 확인 — csp.json 의 Redis.Host 가 있어야 hot replication 가능
    csp_json = os.path.join(ctx.dist_dir, "csp", "csp.json")
    redis_host = ""
    if os.path.isfile(csp_json):
        try:
            d = json.load(open(csp_json))
            redis_host = (d.get("Redis") or {}).get("Host") or ""
        except Exception:
            pass
    if not redis_host:
        return _skip(ctx, "csp.json Redis.Host 미설정 — register hot replication 비활성")

    # LIVE 시나리오 구현은 후속 라운드.
    return _skip(ctx, "2-node + Redis 환경 감지됨 — LIVE 본체 미구현 (1.H 후속)")


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-SCN-FAILOVER-CSP — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-FAILOVER-CSP",
        name="HA fail-over — CSP active kill → 단말 REGISTER 복원",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )


def _peer_sip_ready(ip: str, port: int, timeout: float = 1.0) -> bool:
    """UDP bind 직접 확인은 어려움 — TCP 연결 시도로 호스트 reachable 만 검증."""
    try:
        with socket.create_connection((ip, 22), timeout=timeout):  # ssh 가용성으로 호스트 alive 만
            return True
    except Exception:
        return False
