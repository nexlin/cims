"""S5-CSC-VERIFY (그룹) — 배포된 oam 파일/overlay 검증 (native).

파일 시스템 검증만 — install job 후 산출물 정합성. 외부 의존 없음.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from . import _native_steps


@verify_item(
    id="S5-CSC-VERIFY", stage=5, category="배포",
    name="oam 배포 산출물 검증 (파일/overlay)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=30,
)
def csc_verify_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-CSC-VERIFY", name="oam 배포 산출물 검증 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-CSC-VERIFY-FILES", stage=5, category="배포",
    name="설치 파일 검증 (meta.json + config/)",
    parent="S5-CSC-VERIFY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=30,
    execution_order=31,
)
def verify_files(ctx: VerifyContext) -> ItemResult:
    """Step 11 native — meta.json + config/ 존재 검증."""
    return _native_steps.step_11_verify_files(ctx)


@verify_item(
    id="S5-CSC-VERIFY-OVERLAY", stage=5, category="배포",
    name="config overlay 반영 검증",
    parent="S5-CSC-VERIFY",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=30,
    execution_order=32,
)
def verify_overlay(ctx: VerifyContext) -> ItemResult:
    """Step 12 native — oam/config.json Server.Port=4445 반영 검증."""
    return _native_steps.step_12_verify_overlay(ctx)
