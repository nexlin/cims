"""S5-RESET — Phase 2 step 01 (Cleanup) — native Python 구현 사용."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from . import _native_steps as _native


@verify_item(
    id="S5-RESET",
    stage=5, category="배포",
    name="배포본 reset (로그/DB/csc-server wipe — 가입자 보존)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "db-truncate"], timeout_s=120,
)
def reset(ctx: VerifyContext) -> ItemResult:
    return _native.step_01_cleanup(ctx)
