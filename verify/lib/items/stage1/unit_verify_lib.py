"""S1-UNIT-VERIFY-LIB — verify.lib unit test."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S1-UNIT-VERIFY-LIB",
    stage=1, category="정적",
    name="verify.lib unit test (python3 -m unittest tests.test_verify_lib)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
)
def unit_verify_lib(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_verify_lib.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id="S1-UNIT-VERIFY-LIB", name="verify.lib unit test",
            status=ItemStatus.SKIP,
            detail="tests/test_verify_lib.py 없음", stage=1,
        )
    rc, out, err = shell.run(
        ["python3", "-m", "unittest", "tests.test_verify_lib", "-v"],
        cwd=ctx.repo_root, timeout=60,
    )
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w("## S1-UNIT-VERIFY-LIB — verify.lib unit test")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-UNIT-VERIFY-LIB", name="verify.lib unit test",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
