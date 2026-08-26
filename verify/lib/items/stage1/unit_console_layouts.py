"""S1-UNIT-CONSOLE-LAYOUT — 콘솔 레이아웃 영속 계약 unit test.

위젯 분해 이후 배치 확장 필드(x/y/config/title)와 seed 세대(seedVersion)가 PUT 왕복에서
유실되지 않는지를 지킨다 — 유실되면 저장본이 옛 배치를 영구 고정하거나 개편 안내가
매번 다시 뜬다(console_platform.md §3.3~3.4).
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CONSOLE-LAYOUT"
_NAME = "콘솔 레이아웃 영속 unit test (python3 -m unittest tests.test_console_layouts)"


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=52,
)
def unit_console_layouts(ctx: VerifyContext) -> ItemResult:
    test_module = os.path.join(ctx.repo_root, "tests", "test_console_layouts.py")
    if not os.path.isfile(test_module):
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail="tests/test_console_layouts.py 없음", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", "tests.test_console_layouts"],
            cwd=ctx.repo_root, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=120, text=True,
        )
        rc, out, err = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = -1
        out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
        err = (e.stderr.decode("utf-8", "replace") if e.stderr else "") \
              + f"\n[TIMEOUT after 120s] {e}"
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w(f"## {_ID} — 콘솔 레이아웃 영속 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id=_ID, name=_NAME,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
