"""S6-SCN-FAILOVER-CMP — CMP-A kill → DEAD 판정 후 신규 세션 CMP-B 분산.

multi-CMP 환경 (`csp.json` 의 `Setup.MediaServer.Endpoints` 가 2개 이상) 일
때만 LIVE. 단일 CMP 환경 SKIP.

LIVE 흐름 (기대 동작 — CmpClient per-endpoint 헬스체크):
  1. csp.json Setup.MediaServer.Endpoints 가 ≥2 확인.
  2. CMP-A kill (예: pkill -f 'bin/cmp .*9000').
  3. CSP KeepAliveLoop 가 CMP-A 를 DEAD 로 판정할 때까지 대기 (연속 3회 × 3초 ≈9초).
     → m_ring.MarkUnhealthy → ConsistentHashRing.IsHealthy(CMP-A) == false.
  4. CMP-A 로 진행중이던 호가 CSP 능동 BYE 로 종료됨 확인 (해당 단말 BYE 수신).
  5. cspsim 으로 신규 세션 5개 INVITE → 모두 CMP-B 로 분배 확인
     (CMP-B 로그에 ADD_SESSION 도착 5건; CMP-A 로는 0건).
  6. 복구: CMP-A 재기동 → HEARTBEAT 재응답 시 ring 재편입(MarkHealthy) 확인.
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

    media = ((d.get("Setup") or {}).get("MediaServer")) or {}
    endpoints = media.get("Endpoints") or []
    # 레거시 단일 Host 또는 Endpoints<2 는 single-CMP → SKIP.
    if not isinstance(endpoints, list) or len(endpoints) < 2:
        n = len(endpoints) if isinstance(endpoints, list) else 0
        return _skip(ctx, f"Setup.MediaServer.Endpoints={n} (<2) — single-CMP 환경, SKIP")

    # LIVE 시나리오 본체(kill→DEAD 대기→BYE·분배 확인→복구)는 후속 라운드.
    return _skip(ctx, f"multi-CMP 환경 감지됨 (Endpoints={len(endpoints)}) — LIVE 본체 미구현 (후속)")


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-SCN-FAILOVER-CMP — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-FAILOVER-CMP",
        name="HA fail-over — CMP-A kill → 신규 세션 CMP-B 분산",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )
