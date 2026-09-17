"""S1-UE-UNIT — 단말 SDK 코어 단위시험 (`build/bin/cimsue_test`).

코어의 공개 계약(반환형·C API ABI·floor 정의·SDS/CSC 코덱)을 지키는 gtest 묶음이다.
`ue_sdk.md` §10 이 각 이행 단계의 완료 조건으로 지목한 항목.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-UE-UNIT"
_NAME = "단말 SDK 코어 단위시험 (cimsue_test)"


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=300,
    execution_order=60,
)
def unit_ue_core(ctx: VerifyContext) -> ItemResult:
    exe = p(ctx.repo_root, "build", "bin", "cimsue_test")
    if not os.path.isfile(exe):
        return skip(_ID, _NAME, "build/bin/cimsue_test 없음 — `cd build && make cimsue_test` 후 재실행")
    rc, out, err = shell.run([exe], cwd=ctx.repo_root, timeout=300)
    full = (out + err).splitlines()
    block(ctx, f"{_ID} — 코어 단위시험", full)
    last = next((l for l in reversed(full) if "tests from" in l or "PASSED" in l or "FAILED" in l), "")
    return done(_ID, _NAME, rc == 0, last or f"rc={rc}")
