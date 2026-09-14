"""S1-UNIT-OAM-HTTPSRV — HttpServer 기동 계약(bind 성공 = 기동 성공) unit test.

떠 있는 척하는 프로세스를 막는다. uvicorn 은 EADDRINUSE 를 잡아 로그만 남기고 종료하므로,
bind 확인 없이 준비 완료를 알리면 **HTTP 없이 살아 있는 프로세스**가 된다 — agent liveness 는
통과하고 readiness 만 영구 실패해 자가치유가 불가능해진다(실측 사고). base·oam-svc 공용.
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-OAM-HTTPSRV"
_NAME = "HttpServer 기동 계약 unit test (python3 -m unittest tests.test_httpsrv_bind)"


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=55,
)
def unit_httpsrv_bind(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_httpsrv_bind.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail="tests/test_httpsrv_bind.py 없음", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", "tests.test_httpsrv_bind"],
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
    ctx.w(f"## {_ID} — HttpServer 기동 계약 unit test")
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
