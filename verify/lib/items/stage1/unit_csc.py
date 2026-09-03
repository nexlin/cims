"""S1-UNIT-CSC — CSC 관리 API 단위시험 2종 (DB 없음).
  · tests/test_csc_dispatch_rbac.py     관제 그룹 편입 RBAC — 감청/청취 그룹 편입 = 콘솔 manager 승인 하나,
                                        가입자(DB users = person 전용) 쪽 역할 SQL 이 나가지 않는다(dispatch_center.md §5.3)
  · tests/test_csc_subscription_realm.py 가입 번호 H(A1) 결박 realm 해석 — access_services → csc.json
                                        Provisioning.Services 순(sip_access_security.md §4.1)"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CSC"
_NAME = "CSC 관리 API unit test — 관제 그룹 RBAC·가입 realm (python3 -m unittest tests.test_csc_dispatch_rbac tests.test_csc_subscription_realm)"
_MODULES = ["tests.test_csc_dispatch_rbac", "tests.test_csc_subscription_realm"]


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=52,
)
def unit_csc(ctx: VerifyContext) -> ItemResult:
    missing = [m for m in _MODULES if not os.path.isfile(os.path.join(ctx.repo_root, m.replace(".", "/") + ".py"))]
    if missing:
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail=f"{', '.join(missing)} 없음", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", *_MODULES],
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
    ctx.w(f"## {_ID} — CSC 관리 API unit test (관제 그룹 RBAC · 가입 realm)")
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
