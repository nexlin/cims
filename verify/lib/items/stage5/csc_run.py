"""S5-CSC-RUN (그룹) — 배포된 oam(관리평면) 기동 + health (native).

13: oam Start + mgmt 포트(4445) LISTEN, 14: oam Health check (job 상태 폴링),
15: console 정적 서빙 확인 (oam 단일 오리진 — 별도 console 프로세스 없음).
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-CSC-RUN", stage=5, category="배포",
    name="oam 기동 + health (4445 LISTEN + console 정적)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=180,
    execution_order=40,
)
def csc_run_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-CSC-RUN", name="oam 기동 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-CSC-RUN-CSC-START", stage=5, category="배포",
    name="oam Start job (포트 4445 LISTEN)",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=60,
    execution_order=41,
)
def csc_start(ctx: VerifyContext) -> ItemResult:
    """Step 13 native — oam Start job + mgmt 포트 LISTEN polling 25s."""
    return _native_steps.step_13_csc_start(ctx)


@verify_item(
    id="S5-CSC-RUN-CSC-HEALTH", stage=5, category="배포",
    name="oam Health check job",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
    execution_order=42,
)
def csc_health(ctx: VerifyContext) -> ItemResult:
    """Step 14 native — health_check job + job 상태 폴링 15s."""
    return _native_steps.step_14_csc_health(ctx)


@verify_item(
    id="S5-CSC-RUN-CONSOLE-START", stage=5, category="배포",
    name="console 정적 서빙 (oam 단일 오리진)",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
    execution_order=43,
)
def console_static(ctx: VerifyContext) -> ItemResult:
    """Step 15 native — GET / 가 SPA HTML(200) 인지 확인."""
    return _native_steps.step_15_console_static(ctx)
