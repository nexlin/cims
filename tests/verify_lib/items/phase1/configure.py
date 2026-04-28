"""Phase 1 §4 — configure (ens160 IP 반영)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="P1-CONFIGURE",
    phase=1, category="환경",
    name="configure --local-ip <ens160> (csp/cmp/csc/cwrtc/console 설정 재생성)",
    depends_on=["P1-PREFLIGHT"],
    presets=["phase1-full"],
    side_effects=["fs-write"], timeout_s=60,
)
def configure(ctx: VerifyContext) -> ItemResult:
    """ens160 IP 로 cims.sh configure 호출. IP 미감지 시 127.0.0.1."""
    target_ip = ctx.ens_ip or "127.0.0.1"
    rc, out, err = shell.run_cims_sh(
        ctx.repo_root, "configure", "--local-ip", target_ip, timeout=60,
    )
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w(f"## P1-CONFIGURE — configure --local-ip {target_ip}")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="P1-CONFIGURE", name=f"configure (ip={target_ip})",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, phase=1,
    )
