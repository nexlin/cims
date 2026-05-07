"""S5-CSC-DEPLOY (그룹) — TB-CSC(4419) → csc-server 배포 체인 3단계 (native)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


# ── 그룹 (placeholder) ───────────────────────────────────────────
@verify_item(
    id="S5-CSC-DEPLOY", stage=5, category="배포",
    name="csc-server 배포 (TB-CSC 4419 → Test-agent → install)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "service-state", "network"],
    timeout_s=600,
    execution_order=20,
)
def csc_deploy_group(ctx: VerifyContext) -> ItemResult:
    """그룹 placeholder — runner 가 자식 worst-status 로 합산."""
    return ItemResult(
        id="S5-CSC-DEPLOY", name="csc-server 배포 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


# ── 자식 ────────────────────────────────────────────────────────
@verify_item(
    id="S5-CSC-DEPLOY-AGENT-ENROLL", stage=5, category="배포",
    name="TB-CSC admin login + agent 등록 + Test-agent 기동(9903)",
    parent="S5-CSC-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network", "process-start"], timeout_s=120,
    execution_order=21,
)
def deploy_agent_enroll(ctx: VerifyContext) -> ItemResult:
    """Step 05+06+07 native 합성 (admin login + agent register + Test-agent)."""
    return _native_steps.steps_05_06_07_agent_enroll(ctx)


@verify_item(
    id="S5-CSC-DEPLOY-PKG-UPLOAD", stage=5, category="배포",
    name="csc/console 패키지 업로드 (→ TB-CSC 4419)",
    parent="S5-CSC-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["network"], timeout_s=120,
    execution_order=22,
)
def deploy_pkg_upload(ctx: VerifyContext) -> ItemResult:
    """Step 08 native — csc/console tarball 업로드."""
    return _native_steps.step_08_package_upload(ctx)


@verify_item(
    id="S5-CSC-DEPLOY-INSTALL", stage=5, category="배포",
    name="Deployment 생성 + Install job + 폴링",
    parent="S5-CSC-DEPLOY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "network"], timeout_s=300,
    execution_order=23,
)
def deploy_install(ctx: VerifyContext) -> ItemResult:
    """Step 09+10 native — deployment 생성 + install job + DB 폴링."""
    return _native_steps.steps_09_10_install(ctx)
