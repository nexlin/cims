"""S3-CONFIGURE — configure --local-ip <ens160>."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S3-CONFIGURE",
    stage=3, category="환경",
    name="configure --local-ip <ens160> (csp/cmp/csc/cwrtc/console 설정 재생성)",
    depends_on=["S3-RESET"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write"], timeout_s=60,
    execution_order=20,
)
def configure(ctx: VerifyContext) -> ItemResult:
    target_ip = ctx.ens_ip or "127.0.0.1"
    rc, out, err = shell.run_cims_sh(
        ctx.repo_root, "configure", "--local-ip", target_ip, timeout=60,
    )
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w(f"## S3-CONFIGURE — configure --local-ip {target_ip}")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S3-CONFIGURE", name=f"configure (ip={target_ip})",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=3,
    )
