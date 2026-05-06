"""S4-PKG-BUILD — cims.sh pkg --no-bump (5개 tarball 생성)."""
from __future__ import annotations

import os
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S4-PKG-BUILD",
    stage=4, category="패키지",
    name="패키지 빌드 (cims.sh pkg --no-bump → build/dist/packages/*.tar.gz)",
    presets=["stage4-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write"], timeout_s=300,
)
def pkg_build(ctx: VerifyContext) -> ItemResult:
    if ctx.skip_pkg:
        ctx.w("## S4-PKG-BUILD — SKIPPED (skip_pkg=true)")
        ctx.w()
        return ItemResult(
            id="S4-PKG-BUILD", name="패키지 빌드",
            status=ItemStatus.SKIP, detail="skip_pkg=true", stage=4,
        )
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "pkg", "--no-bump", timeout=300)
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-25:])

    pkg_dir = os.path.join(ctx.dist_dir, "packages")
    tarballs = sorted(glob(os.path.join(pkg_dir, "*.tar.gz")))
    n = len(tarballs)

    ctx.w("## S4-PKG-BUILD — pkg --no-bump")
    ctx.w(f"- 산출물: {n}개 tarball ({pkg_dir})")
    for t in tarballs:
        size = os.path.getsize(t)
        ctx.w(f"  - {os.path.basename(t)} ({size:,} bytes)")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()

    # 5개 모듈 (csc/csp/cmp/sim/console) 모두 생성되어야 PASS
    ok = rc == 0 and n >= 5
    return ItemResult(
        id="S4-PKG-BUILD", name="패키지 빌드",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"rc={rc}, tarballs={n}\n{tail}", stage=4,
    )
