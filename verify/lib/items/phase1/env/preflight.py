"""Phase 1 §1 — preflight (cims.sh cmd_preflight 위임)."""
from __future__ import annotations

from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext
from .... import shell


@verify_item(
    id="P1-PREFLIGHT",
    phase=1, category="환경",
    name="preflight (ens160 IP / git / 포트 / DB)",
    presets=["phase1-full", "phase1-quick"],
    side_effects=["read-only"], timeout_s=30,
)
def preflight(ctx: VerifyContext) -> ItemResult:
    """cims.sh cmd_preflight 호출 + stdout tail 캡처."""
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "preflight", timeout=30)
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w("## P1-PREFLIGHT — preflight")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="P1-PREFLIGHT", name="preflight",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, phase=1,
    )
