"""S5-MODULES-DEPLOY (그룹) — 배포본 OAM(4445) → csc + service-server 배포 체인 (native).

step 16 (admin login) / 17 (3 tarball 업로드) / 18 (3 agent + 3 Test-agent
spawn) / 19+20 (3 deployment + install poll).
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-MODULES-DEPLOY", stage=5, category="배포",
    name="csc + csp/cmp 배포 (배포본 OAM 4445 경유)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "service-state", "network"],
    timeout_s=600,
    execution_order=50,
)
def modules_deploy_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-MODULES-DEPLOY", name="모듈 배포 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-MODULES-DEPLOY-AUTH", stage=5, category="배포",
    name="배포본 OAM admin login (4445)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
    execution_order=51,
)
def modules_auth(ctx: VerifyContext) -> ItemResult:
    """Step 16 native — 배포본 OAM(4445) admin login → tok2."""
    return _native_steps.step_16_modules_auth(ctx)


@verify_item(
    id="S5-MODULES-DEPLOY-PKG-UPLOAD", stage=5, category="배포",
    name="csc/csp/cmp 패키지 업로드 (→ 배포본 OAM 4445)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=120,
    execution_order=52,
)
def modules_pkg_upload(ctx: VerifyContext) -> ItemResult:
    """Step 17 native — 모듈 tarball 업로드 (csc + service-server)."""
    return _native_steps.step_17_modules_pkg_upload(ctx)


@verify_item(
    id="S5-MODULES-DEPLOY-AGENT-ENROLL", stage=5, category="배포",
    name="csc/csp/cmp agent 등록 + Test-agent 기동 (9904~9909)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network", "process-start"], timeout_s=120,
    execution_order=53,
)
def modules_agent_enroll(ctx: VerifyContext) -> ItemResult:
    """Step 18 native — agent 등록 + Test-agent spawn + enroll 폴링."""
    return _native_steps.step_18_modules_agent_enroll(ctx)


@verify_item(
    id="S5-MODULES-DEPLOY-INSTALL", stage=5, category="배포",
    name="Deployment 생성 + Install job (csc + service-server)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "network"], timeout_s=300,
    execution_order=54,
)
def modules_install(ctx: VerifyContext) -> ItemResult:
    """Step 19+20 native — 3 deployment 생성 + install job + 상태 폴링."""
    return _native_steps.steps_19_20_modules_install(ctx)
