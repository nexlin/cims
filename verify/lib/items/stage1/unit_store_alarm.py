"""S1-UNIT-STORE-ALARM — 공유 store 접근 불가 알람(A-PRC-028) 감지·평가 unit test.

멈춘 NFS 에서 probe 가 호출자를 붙들지 않는지(감지기 자기보호)와, 규칙이 카탈로그
정의와 일치하는지를 본다. 이 구간이 무알람이면 모듈만 조용히 멈춘다 — 실측 사고.
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-STORE-ALARM"
_NAME = "공유 store 알람(A-PRC-028) unit test (python3 -m unittest tests.test_store_alarm)"


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=54,
)
def unit_store_alarm(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_store_alarm.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail="tests/test_store_alarm.py 없음", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", "tests.test_store_alarm"],
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
    ctx.w(f"## {_ID} — 공유 store 알람(A-PRC-028) unit test")
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
