"""S6-SCN-FAILOVER-CSC — CSC active 강제 종료 → Console 재연결.

Phase 1.H stub. 2-node 환경 (`agent/keepalived/ha.json` 존재 + standby peer
응답) 일 때만 LIVE. 그 외 (현재 dev/single-node) SKIP.

LIVE 흐름:
  1. ha.json 의 peer_ip 가 4420 응답 → 2-node 환경 확인.
  2. active CSC process kill (cims.sh stop csc).
  3. ≤5s 대기 후 peer 의 4420/admin API 200 OK 응답 확인 → VIP 인수 + CSC standby 승격.
  4. 복구: 원래 active 노드 cims.sh start csc.
"""
from __future__ import annotations

import json
import os
import socket

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_HA_JSON_REL = ("agent", "keepalived", "ha.json")


@verify_item(
    id="S6-SCN-FAILOVER-CSC", stage=6, category="시나리오",
    name="HA fail-over — CSC active kill → Console 재연결",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full"],
    side_effects=["process-state", "network"], timeout_s=30,
    execution_order=95,
)
def scn_failover_csc(ctx: VerifyContext) -> ItemResult:
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
    if not _peer_admin_ready(peer_ip, 4420):
        return _skip(ctx, f"peer {peer_ip}:4420 응답 없음 — 2-node 환경 미준비")

    # LIVE 시나리오 구현은 후속 라운드 (실제 fail-over 검증 환경 마련 후).
    return _skip(ctx, "2-node 환경 감지됨 — LIVE 본체 미구현 (1.H 후속)")


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-SCN-FAILOVER-CSC — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-FAILOVER-CSC",
        name="HA fail-over — CSC active kill → Console 재연결",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )


def _peer_admin_ready(ip: str, port: int, timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except Exception:
        return False
