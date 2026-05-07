"""S5-RESET — step 01 (Cleanup) native Python 구현.

`_native_steps.step_01_cleanup` 으로 cmd_reset 직접 호출. stage5-full 실행 시
execution_order=10 으로 deploy 체인보다 먼저 실행되도록 명시.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-RESET",
    stage=5, category="배포",
    name="배포본 reset (로그/DB/csc-server wipe — 가입자 보존)",
    # 검증 회차에서는 제외 — `prep-reset` preset 으로 사용자가 명시 실행 (분리).
    presets=["prep-reset"],
    side_effects=["fs-write", "db-truncate"], timeout_s=120,
    execution_order=10,
)
def reset(ctx: VerifyContext) -> ItemResult:
    return _native_steps.step_01_cleanup(ctx)
