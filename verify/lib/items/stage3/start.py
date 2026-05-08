"""S3-START — start all (cmp → csp → cwrtc → csc → console → phone)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S3-START",
    stage=3, category="환경",
    name="start all (cmp → csp → cwrtc → csc → console → phone)",
    depends_on=["S3-CONFIGURE"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["service-start"], timeout_s=120,
    execution_order=30,
)
def start(ctx: VerifyContext) -> ItemResult:
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "start", timeout=120)
    tail = "\n".join((out + err).splitlines()[-40:])
    ctx.w("## S3-START — start all")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S3-START", name="start all",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=3,
    )
