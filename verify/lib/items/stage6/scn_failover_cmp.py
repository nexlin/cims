"""S6-SCN-FAILOVER-CMP — CMP-A kill → 신규 세션 CMP-B 분산.

Phase 1.H stub. multi-CMP 환경 (`csp.json` Cmp.Endpoints 가 2개 이상) 일
때만 LIVE. 현재 dev 환경 (단일 CMP) SKIP.

LIVE 흐름:
  1. csp.json Cmp.Endpoints 가 ≥2 확인.
  2. CMP-A kill (예: pkill -f 'bin/cmp .*9000').
  3. cspsim 으로 신규 세션 5개 INVITE → 모두 CMP-B 로 분배 확인
     (CMP-B 로그에 ADD_SESSION 도착 5건).
  4. ConsistentHashRing.IsHealthy(CMP-A) == false 확인 (관찰자 endpoint).
  5. 복구: CMP-A 재기동, 30초 후 healthy 복귀 확인.
"""
from __future__ import annotations

import json
import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


@verify_item(
    id="S6-SCN-FAILOVER-CMP", stage=6, category="시나리오",
    name="HA fail-over — CMP-A kill → 신규 세션 CMP-B 분산",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full"],
    side_effects=["process-state", "network"], timeout_s=30,
    execution_order=97,
)
def scn_failover_cmp(ctx: VerifyContext) -> ItemResult:
    csp_json = os.path.join(ctx.dist_dir, "csp", "csp.json")
    if not os.path.isfile(csp_json):
        return _skip(ctx, f"csp.json 없음 ({csp_json})")
    try:
        d = json.load(open(csp_json))
    except Exception as e:
        return _skip(ctx, f"csp.json 파싱 실패: {e}")

    cmp_section = d.get("Cmp") or {}
    endpoints = cmp_section.get("Endpoints") or []
    if not endpoints or len(endpoints) < 2:
        return _skip(ctx, f"Cmp.Endpoints={len(endpoints)} (<2) — single-CMP 환경, SKIP")

    # LIVE 시나리오 구현은 후속 라운드.
    return _skip(ctx, f"multi-CMP 환경 감지됨 (Endpoints={len(endpoints)}) — LIVE 본체 미구현 (1.H 후속)")


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-SCN-FAILOVER-CMP — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-FAILOVER-CMP",
        name="HA fail-over — CMP-A kill → 신규 세션 CMP-B 분산",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )
