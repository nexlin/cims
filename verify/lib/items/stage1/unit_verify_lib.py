"""S1-UNIT-VERIFY-LIB — verify.lib unit test."""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


@verify_item(
    id="S1-UNIT-VERIFY-LIB",
    stage=1, category="정적",
    name="verify.lib unit test (python3 -m unittest tests.test_verify_lib)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=50,
)
def unit_verify_lib(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_verify_lib.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id="S1-UNIT-VERIFY-LIB", name="verify.lib unit test",
            status=ItemStatus.SKIP,
            detail="tests/test_verify_lib.py 없음", stage=1,
        )
    # `-v` 옵션 제거 + PYTHONWARNINGS=ignore::ResourceWarning:
    # verbose 시 161 testcase × line + ResourceWarning 다수가 stdout/stderr 에
    # ~100KB 누적되어 subprocess.PIPE buffer (~64KB) 가 가득차 child process
    # write blocking → timeout 60s 도달. 디폴트 출력은 dot/요약뿐이라 안전.
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", "tests.test_verify_lib"],
            cwd=ctx.repo_root, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=120, text=True,
        )
        rc, out, err = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = -1
        out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
        err = (e.stderr.decode("utf-8", "replace") if e.stderr else "") \
              + f"\n[TIMEOUT after 120s] {e}"
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
