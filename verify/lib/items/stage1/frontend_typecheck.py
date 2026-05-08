"""S1-FRONTEND-TYPECHECK — cims-console TypeScript 타입 체크 (tsc -b --noEmit)."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S1-FRONTEND-TYPECHECK",
    stage=1, category="정적",
    name="cims-console TypeScript typecheck (tsc -b --noEmit)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=180,
    execution_order=30,
)
def frontend_typecheck(ctx: VerifyContext) -> ItemResult:
    console_dir = os.path.join(ctx.repo_root, "cims-console")
    if not os.path.isdir(os.path.join(console_dir, "node_modules")):
        return ItemResult(
            id="S1-FRONTEND-TYPECHECK", name="cims-console typecheck",
            status=ItemStatus.SKIP,
            detail="cims-console/node_modules 없음 — `npm install` 선행 필요", stage=1,
        )
    # npx tsc 사용 — package.json 의 typecheck script 가 없을 수도 있어 직접 실행
    rc, out, err = shell.run(
        ["npx", "--no-install", "tsc", "-b", "--noEmit"],
        cwd=console_dir, timeout=180,
    )
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-40:])
    ctx.w("## S1-FRONTEND-TYPECHECK — TypeScript typecheck")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-FRONTEND-TYPECHECK", name="cims-console typecheck",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
