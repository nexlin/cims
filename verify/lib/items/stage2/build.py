"""S2-BUILD — cmake build (dist 만 — tarball 은 S4-PKG-BUILD)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S2-BUILD",
    stage=2, category="환경",
    name="build (dist only — tarball 미생성)",
    depends_on=["S2-PREFLIGHT"],
    presets=["stage2-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write"], timeout_s=900,
)
def build(ctx: VerifyContext) -> ItemResult:
    if ctx.skip_build:
        ctx.w("## S2-BUILD — SKIPPED (skip_build=true)")
        ctx.w()
        return ItemResult(
            id="S2-BUILD", name="build (dist only)",
            status=ItemStatus.SKIP, detail="skip_build=true", stage=2,
        )
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "build", timeout=900)
    tail = "\n".join((out + err).splitlines()[-40:])
    ctx.w("## S2-BUILD — build (dist only)")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S2-BUILD", name="build (dist only)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=2,
    )
