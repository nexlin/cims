"""S5-FINALIZE — step 22 (Stop / 전체 기동 유지) native.

`--stop-after` 옵션 시 모든 deployment stop + Test-agent 4개 kill. 기본은
정보성 detail (Phase 3 진입 위해 4 ports 기동 유지).
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-FINALIZE",
    stage=5, category="배포",
    name="배포 마무리 (전체 기동 유지 / --stop-after 시 Stop job)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-state"], timeout_s=60,
    execution_order=70,
)
def finalize(ctx: VerifyContext) -> ItemResult:
    """Step 22 native — `--stop-after` 면 stop + kill, 기본은 기동 유지."""
    return _native_steps.step_22_finalize(ctx)
