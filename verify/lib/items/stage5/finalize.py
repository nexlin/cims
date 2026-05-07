"""S5-FINALIZE — Phase 2 step 22 (Stop / 전체 기동 유지)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-FINALIZE",
    stage=5, category="배포",
    name="배포 마무리 (전체 기동 유지 / --stop-after 시 Stop job)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-state"], timeout_s=60,
    execution_order=70,
)
def finalize(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [22], "S5-FINALIZE",
                       "전체 기동 유지 / Stop job")
