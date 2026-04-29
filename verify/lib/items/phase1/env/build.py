"""Phase 1 §3 — build (cims.sh cmd_build 위임). dist 만 빌드 — tarball 은 Phase 2."""
from __future__ import annotations

from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext
from .... import shell


@verify_item(
    id="P1-BUILD",
    phase=1, category="환경",
    name="build (dist only — tarball 미생성)",
    depends_on=["P1-RESET"],
    presets=["phase1-full"],
    side_effects=["fs-write"], timeout_s=900,
)
def build(ctx: VerifyContext) -> ItemResult:
    """skip_build 옵션 시 SKIP. 그 외 cims.sh build."""
    if ctx.skip_build:
        ctx.w("## P1-BUILD — SKIPPED (skip_build=true)")
        ctx.w()
        return ItemResult(
            id="P1-BUILD", name="build (dist only)",
            status=ItemStatus.SKIP, detail="skip_build=true", phase=1,
        )
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "build", timeout=900)
    tail = "\n".join((out + err).splitlines()[-40:])
    ctx.w("## P1-BUILD — build (dist only)")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="P1-BUILD", name="build (dist only)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, phase=1,
    )
