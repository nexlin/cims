"""S1-FRONTEND-LINT — 콘솔 eslint."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S1-FRONTEND-LINT",
    stage=1, category="정적",
    name="콘솔 eslint (npm run lint)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=20,
)
def frontend_lint(ctx: VerifyContext) -> ItemResult:
    # 콘솔은 ems/core/console (base 셸) — 서비스 팩(ems/service/console)은 @svc alias 로
    # 같은 tsconfig/eslint 에 함께 걸리므로 이 한 곳에서 검사하면 콘솔 전체가 덮인다.
    console_dir = os.path.join(ctx.repo_root, "ems", "core", "console")
    if not os.path.isfile(os.path.join(console_dir, "package.json")):
        return ItemResult(
            id="S1-FRONTEND-LINT", name="콘솔 eslint",
            status=ItemStatus.SKIP, detail="ems/core/console/package.json 없음", stage=1,
        )
    rc, out, err = shell.run(
        ["npm", "run", "--silent", "lint"],
        cwd=console_dir, timeout=120,
    )
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-40:])
    ctx.w("## S1-FRONTEND-LINT — 콘솔 eslint")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-FRONTEND-LINT", name="콘솔 eslint",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
