"""S5-MODULES-RUN (그룹) — 배포된 csp/cmp 기동.

자식 모두 native 화 완료 (_legacy 미참조). step 21 native — Start 가 PASS 일 때
.deployed-manifest.json marker 도 step 21 안에서 자동 기록 (S6-ENTRY-CHECK
immutability gate).
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-MODULES-RUN", stage=5, category="배포",
    name="csp/cmp Start (5060/udp + 9000/udp LISTEN — sim install-only)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=180,
    execution_order=60,
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
    execution_order=61,
)
def modules_start(ctx: VerifyContext) -> ItemResult:
    """Step 21 native — csp/cmp Start + LISTEN 확인 + immutability marker."""
    return _native_steps.step_21_modules_start(ctx)
