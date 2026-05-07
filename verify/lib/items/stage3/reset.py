"""S3-RESET — dev 환경 wipe (가입자 보존)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S3-RESET",
    stage=3, category="환경",
    name="reset --all (가입자 보존, 로그/DB/녹취/배포본 wipe)",
    # 검증 회차에서는 제외 — `prep-reset` preset 으로 사용자가 명시 실행 (분리).
    # stage3/5/pipeline-full preset 에는 미포함.
    presets=["prep-reset"],
    side_effects=["fs-write", "db-truncate", "process-kill"], timeout_s=120,
)
def reset(ctx: VerifyContext) -> ItemResult:
    if ctx.skip_reset:
        ctx.w("## S3-RESET — SKIPPED (skip_reset=true)")
        ctx.w()
        return ItemResult(
            id="S3-RESET", name="reset (가입자 보존)",
            status=ItemStatus.SKIP, detail="skip_reset=true", stage=3,
        )
    rc, out, err = shell.run_cims_sh(ctx.repo_root, "reset", "--all", timeout=120)
    tail = "\n".join((out + err).splitlines()[-30:])
    ctx.w("## S3-RESET — reset --all")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S3-RESET", name="reset (가입자 보존)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=3,
    )
