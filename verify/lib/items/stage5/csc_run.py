"""S5-CSC-RUN (그룹) — 배포된 csc/console 기동 + health."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


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
    by = get_legacy_results(ctx)
    return step_result(by, [13], "S5-CSC-RUN-CSC-START", "csc Start (4445 LISTEN)")


@verify_item(
    id="S5-CSC-RUN-CSC-HEALTH", stage=5, category="배포",
    name="csc Health check job",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
    execution_order=42,
)
def csc_health(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [14], "S5-CSC-RUN-CSC-HEALTH", "csc health_check")


@verify_item(
    id="S5-CSC-RUN-CONSOLE-START", stage=5, category="배포",
    name="console Start job (포트 8081 LISTEN)",
    parent="S5-CSC-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=60,
    execution_order=43,
)
def console_start(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [15], "S5-CSC-RUN-CONSOLE-START",
                       "console Start (8081 LISTEN)")
