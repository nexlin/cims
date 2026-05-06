"""S5-MODULES-DEPLOY (그룹) — 배포본 csc(4445) → csp/cmp/cspsim 배포 체인."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-MODULES-DEPLOY", stage=5, category="배포",
    name="csp/cmp/cspsim 배포 (배포본 csc 4445 경유)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "service-state", "network"],
    timeout_s=600,
)
def modules_deploy_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-MODULES-DEPLOY", name="모듈 배포 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-MODULES-DEPLOY-AUTH", stage=5, category="배포",
    name="배포본 csc admin login (4445)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=30,
)
def modules_auth(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [16], "S5-MODULES-DEPLOY-AUTH",
                       "배포본 csc admin login")


@verify_item(
    id="S5-MODULES-DEPLOY-PKG-UPLOAD", stage=5, category="배포",
    name="csp/cmp/cspsim 패키지 업로드 (→ 배포본 csc 4445)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=120,
)
def modules_pkg_upload(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [17], "S5-MODULES-DEPLOY-PKG-UPLOAD",
                       "csp/cmp/sim 패키지 업로드")


@verify_item(
    id="S5-MODULES-DEPLOY-AGENT-ENROLL", stage=5, category="배포",
    name="csp/cmp/sim agent 등록 + Test-agent 기동 (9904/5/6)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network", "process-start"], timeout_s=120,
)
def modules_agent_enroll(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [18], "S5-MODULES-DEPLOY-AGENT-ENROLL",
                       "agent 등록 + Test-agent 기동")


@verify_item(
    id="S5-MODULES-DEPLOY-INSTALL", stage=5, category="배포",
    name="Deployment 생성 + Install job (csp/cmp/cspsim)",
    parent="S5-MODULES-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "network"], timeout_s=300,
)
def modules_install(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [19, 20], "S5-MODULES-DEPLOY-INSTALL",
                       "Deployment + Install (csp/cmp/sim)")
