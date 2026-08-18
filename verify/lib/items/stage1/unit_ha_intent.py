"""S1-UNIT-HA-INTENT — HA 무장/해제 의도 계약 + keepalived 소유 경계 unit test."""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-HA-INTENT"
_NAME = "HA intent / 소유 경계 unit test (python3 -m unittest tests.test_ha_intent)"


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=51,
)
def unit_ha_intent(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_ha_intent.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail="tests/test_ha_intent.py 없음", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", "tests.test_ha_intent"],
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
    ctx.w(f"## {_ID} — HA intent / 소유 경계 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id=_ID, name=_NAME,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
