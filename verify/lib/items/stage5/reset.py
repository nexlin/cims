"""S5-RESET — 배포본 reset (Phase 2 step 1: cleanup)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-RESET",
    stage=5, category="배포",
    name="배포본 reset (로그/DB/csc-server wipe — 가입자 보존)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "db-truncate"], timeout_s=60,
)
def reset(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [1], "S5-RESET", "배포본 reset (cleanup)")
