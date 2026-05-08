"""S1-FRONTEND-LINT — cims-console eslint."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S1-FRONTEND-LINT",
    stage=1, category="정적",
    name="cims-console eslint (npm run lint)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=20,
)
def frontend_lint(ctx: VerifyContext) -> ItemResult:
    console_dir = os.path.join(ctx.repo_root, "cims-console")
    if not os.path.isfile(os.path.join(console_dir, "package.json")):
        return ItemResult(
            id="S1-FRONTEND-LINT", name="cims-console eslint",
            status=ItemStatus.SKIP, detail="cims-console/package.json 없음", stage=1,
        )
    rc, out, err = shell.run(
        ["npm", "run", "--silent", "lint"],
        cwd=console_dir, timeout=120,
    )
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-40:])
    ctx.w("## S1-FRONTEND-LINT — cims-console eslint")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-FRONTEND-LINT", name="cims-console eslint",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
