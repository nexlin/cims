"""S5-RESET — Phase 2 step 01 (Cleanup) — native Python 구현.

`_native_steps.step_01_cleanup` 으로 cmd_reset 직접 호출. stage5-full 실행 시
execution_order=10 으로 deploy 체인보다 먼저 실행되도록 명시 (이전 alphabetical
회귀: S5-RESET 이 마지막에 실행되어 deploy 산출물 wipe).

_legacy.get_legacy_results 의 _verify_phase2 본체도 step 1 (cleanup) 을 실행하
므로 중복 실행이 발생하나 cmd_reset 은 idempotent. step 02~22 가 native 로
포팅되면 _verify_phase2 호출 자체가 사라짐.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-RESET",
    stage=5, category="배포",
    name="배포본 reset (로그/DB/csc-server wipe — 가입자 보존)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "db-truncate"], timeout_s=120,
    execution_order=10,
)
def reset(ctx: VerifyContext) -> ItemResult:
    return _native_steps.step_01_cleanup(ctx)
