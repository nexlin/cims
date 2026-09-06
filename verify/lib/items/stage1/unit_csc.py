"""S1-UNIT-CSC — CSC 관리 API·GMS·프로비저닝 단위시험 4종 (DB 없음).
  · tests/test_csc_dispatch_rbac.py       관제 그룹 편입 RBAC — 감청/청취 그룹 편입 = 콘솔 manager 승인 하나,
                                          가입자(DB users = person 전용) 쪽 역할 SQL 이 나가지 않는다(dispatch_center.md §5.3)
  · tests/test_csc_subscription_realm.py  가입 번호 H(A1) 결박 realm 해석 — access_services → csc.json
                                          Provisioning.Services 순(sip_access_security.md §4.1)
  · tests/test_csc_gms_group_crud.py      GMS XCAP 그룹 CRUD(가입자 주체) — 문서 파서 왕복·식별자 검증·인가 게이트·
                                          가짜 DB SQL 조립(mcptt_authorization.md §4.1)
  · tests/test_csc_provisioning_dispatch.py /provisioning/me 관제 데스크 발견 블록 — members/pttTargets 범위 해석·
                                          etag·If-None-Match 304(dispatch_center.md §8.4)
  · tests/test_csc_provisioning_history.py /provisioning/history 통합 이력 — call/ptt/message 범위 게이트·
                                          커서(since)·감사 E-AUD-016(dispatch_center.md §5.6)"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CSC"
_NAME = ("CSC unit test — 관제 그룹 RBAC·가입 realm·GMS 그룹 CRUD·프로비저닝 발견 "
         "(python3 -m unittest tests.test_csc_dispatch_rbac tests.test_csc_subscription_realm "
         "tests.test_csc_gms_group_crud tests.test_csc_provisioning_dispatch)")
_MODULES = ["tests.test_csc_dispatch_rbac", "tests.test_csc_subscription_realm",
            "tests.test_csc_gms_group_crud", "tests.test_csc_provisioning_dispatch",
            "tests.test_csc_provisioning_history"]


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
    ctx.w(f"## {_ID} — CSC unit test (관제 그룹 RBAC · 가입 realm · GMS 그룹 CRUD · 프로비저닝 발견)")
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
