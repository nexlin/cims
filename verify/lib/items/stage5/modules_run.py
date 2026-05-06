"""S5-MODULES-RUN (그룹) — 배포된 csp/cmp 기동."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-MODULES-RUN", stage=5, category="배포",
    name="csp/cmp Start (5060/udp + 9000/udp LISTEN — sim install-only)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=180,
)
def modules_run_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-MODULES-RUN", name="모듈 기동 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-MODULES-RUN-START", stage=5, category="배포",
    name="csp/cmp Start (5060/udp + 9000/udp)",
    parent="S5-MODULES-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=120,
)
def modules_start(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [21], "S5-MODULES-RUN-START",
                       "csp/cmp Start (sim install-only)")
