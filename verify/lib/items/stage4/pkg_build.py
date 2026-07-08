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
    execution_order=10,
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

    # 기대 컴포넌트 — cims.sh:1826 cmd_pkg 의 default targets 와 동기.
    # 변경 시 양쪽 모두 갱신 필요.
    EXPECTED = {
        "cmp", "pmp", "imp",       # 미디어 (VoLTE/PTT/IBCF)
        "cmdp",                    # MCData media plane (MSRP)
        "csp", "psp", "isp",       # 시그널링 (VoLTE/PTT/IBCF)
        "cwrtc", "csc", "console", "phone", "cspsim", "agent",
    }
    # 파일명 `<name>-<ver>.tar.gz` 에서 컴포넌트 이름 추출.
    present = {os.path.basename(t).rsplit("-", 1)[0] for t in tarballs}
    missing = sorted(EXPECTED - present)

    ctx.w("## S4-PKG-BUILD — pkg --no-bump")
    ctx.w(f"- 산출물: {n}개 tarball ({pkg_dir})")
    for t in tarballs:
        size = os.path.getsize(t)
        ctx.w(f"  - {os.path.basename(t)} ({size:,} bytes)")
    if missing:
        ctx.w(f"- **누락 컴포넌트**: {', '.join(missing)}")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()

    # 12개 기대 컴포넌트 전부 존재 + rc==0 이어야 PASS.
    ok = rc == 0 and not missing
    return ItemResult(
        id="S4-PKG-BUILD", name="패키지 빌드",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=(
            f"rc={rc}, tarballs={n}, expected={len(EXPECTED)}, "
            f"missing={missing}\n{tail}"
        ),
        stage=4,
    )
