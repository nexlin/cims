"""Phase 1 §2 — reset (가입자 보존 wipe)."""
from __future__ import annotations

from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext
from .... import shell


@verify_item(
    id="P1-RESET",
    phase=1, category="환경",
    name="reset --all (가입자 보존, 로그/DB/녹취/배포본 wipe)",
    depends_on=["P1-PREFLIGHT"],
    presets=["phase1-full"],
    side_effects=["fs-write", "db-truncate", "process-kill"], timeout_s=120,
)
def reset(ctx: VerifyContext) -> ItemResult:
    """skip_reset 옵션 시 SKIP. 그 외 cims.sh reset --all."""
    if ctx.skip_reset:
        ctx.w("## P1-RESET — SKIPPED (skip_reset=true)")
        ctx.w()
        return ItemResult(
            id="P1-RESET", name="reset (가입자 보존)",
            status=ItemStatus.SKIP, detail="skip_reset=true", phase=1,
        )
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "reset", "--all", timeout=120)
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w("## P1-RESET — reset --all")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="P1-RESET", name="reset (가입자 보존)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, phase=1,
    )
