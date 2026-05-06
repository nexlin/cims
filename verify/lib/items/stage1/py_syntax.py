"""S1-PY-SYNTAX — Python 코드 위생 (py_compile)."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


# repo_root 기준 상대 경로 — Python 코드가 사는 주요 디렉토리
_TARGET_DIRS = [
    "verify",
    "tests",
    "csc/src",
    "cmp_console/scripts",
    "scripts",
]


@verify_item(
    id="S1-PY-SYNTAX",
    stage=1, category="정적",
    name="Python 문법 검사 (py_compile, verify/+tests/+csc/+scripts/)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
)
def py_syntax(ctx: VerifyContext) -> ItemResult:
    targets = []
    for d in _TARGET_DIRS:
        full = os.path.join(ctx.repo_root, d)
        if os.path.isdir(full):
            targets.append(full)
    if not targets:
        return ItemResult(
            id="S1-PY-SYNTAX", name="Python 문법 검사",
            status=ItemStatus.SKIP, detail="대상 디렉토리 없음", stage=1,
        )
    cmd = ["python3", "-m", "compileall", "-q"] + targets
    rc, out, err = shell.run(cmd, cwd=ctx.repo_root, timeout=60)
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w("## S1-PY-SYNTAX — Python 문법 검사")
    ctx.w(f"- 대상: {' '.join(_TARGET_DIRS)}")
    if tail:
        ctx.w("```")
        for line in tail.splitlines(): ctx.w(line)
        ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-PY-SYNTAX", name="Python 문법 검사",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail or f"rc={rc}", stage=1,
    )
