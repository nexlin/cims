"""S5-CSC-RUN (그룹) — 배포된 csc/console 기동 + health.

자식 모두 native 화 완료 (_legacy 미참조). step 13/14/15 native:
- 13: csc Start + 4445 LISTEN
- 14: csc Health check (agent_job 폴링)
- 15: console Start + 8081 LISTEN
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-CSC-RUN", stage=5, category="배포",
    name="csc / console 기동 + health (4445/8081 LISTEN)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=180,
    execution_order=40,
)
def csc_run_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-CSC-RUN", name="csc / console 기동 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-CSC-RUN-CSC-START", stage=5, category="배포",
    name="csc Start job (포트 4445 LISTEN)",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=60,
    execution_order=41,
)
def csc_start(ctx: VerifyContext) -> ItemResult:
    """Step 13 native — csc Start job + 4445 LISTEN polling 25s."""
    return _native_steps.step_13_csc_start(ctx)


@verify_item(
    id="S5-CSC-RUN-CSC-HEALTH", stage=5, category="배포",
    name="csc Health check job",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
    execution_order=42,
)
def csc_health(ctx: VerifyContext) -> ItemResult:
    """Step 14 native — health_check job + agent_job DB 폴링 15s."""
    return _native_steps.step_14_csc_health(ctx)


@verify_item(
    id="S5-CSC-RUN-CONSOLE-START", stage=5, category="배포",
    name="console Start job (포트 8081 LISTEN)",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=60,
    execution_order=43,
)
def console_start(ctx: VerifyContext) -> ItemResult:
    """Step 15 native — console Start job + 8081 LISTEN polling 25s."""
    return _native_steps.step_15_console_start(ctx)
