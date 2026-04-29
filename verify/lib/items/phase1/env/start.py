"""Phase 1 §5 — start all (cmp → csp → cwrtc → csc → console → phone)."""
from __future__ import annotations

from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext
from .... import shell


@verify_item(
    id="P1-START",
    phase=1, category="환경",
    name="start all (cmp → csp → cwrtc → csc → console → phone)",
    depends_on=["P1-CONFIGURE"],
    presets=["phase1-full"],
    side_effects=["service-start"], timeout_s=120,
)
def start(ctx: VerifyContext) -> ItemResult:
    """cims.sh start 호출. 모든 모듈 기동 + status 출력."""
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "start", timeout=120)
    tail = "\n".join((out + err).splitlines()[-40:])
    ctx.w("## P1-START — start all")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="P1-START", name="start all",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, phase=1,
    )
