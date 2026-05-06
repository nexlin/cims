"""S2-PREFLIGHT — preflight (cims.sh cmd_preflight 위임).

ens160 IP / git / 포트 / DB 사전 점검.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S2-PREFLIGHT",
    stage=2, category="환경",
    name="preflight (ens160 IP / git / 포트 / DB)",
    presets=["stage2-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=30,
)
def preflight(ctx: VerifyContext) -> ItemResult:
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "preflight", timeout=30)
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w("## S2-PREFLIGHT — preflight")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S2-PREFLIGHT", name="preflight",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=2,
    )
