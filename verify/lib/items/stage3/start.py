"""S3-START — restart all (cmp → cmdp → csp → oam → csc → console).

`start` 가 아니라 `restart` 다. 직전 S3-CONFIGURE 가 JWT 시크릿·CSP↔CSC 내부 API 토큰을 새로
렌더하므로, 이미 떠 있는 프로세스를 건너뛰는 `start` 로는 구 시크릿을 든 프로세스를 검증하게
된다(내부 API 401 → AKA/XCAP-ROOT 오탐 FAIL, 콘솔 csc 프록시 401). 검증은 "지금 렌더된 설정으로
기동된 스택"이 전제다.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S3-START",
    stage=3, category="환경",
    name="restart all (cmp → cmdp → csp → oam → csc → console — configure 로 렌더된 설정으로 기동 보장)",
    depends_on=["S3-CONFIGURE"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["service-start"], timeout_s=120,
    execution_order=30,
)
def start(ctx: VerifyContext) -> ItemResult:
    rc, out, err = shell.run_cims_svc(ctx.repo_root, "restart", timeout=120)
    tail = "\n".join((out + err).splitlines()[-40:])
    ctx.w("## S3-START — restart all (렌더된 설정으로 기동)")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S3-START", name="restart all",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=3,
    )
