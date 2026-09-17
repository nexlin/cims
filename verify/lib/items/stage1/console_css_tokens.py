"""S1-CONSOLE-CSS-TOKENS — 콘솔이 쓰는 CSS 변수가 실제로 정의돼 있는지.

없는 변수를 쓰면 그 선언이 통째로 무효가 된다(invalid at computed-value time) — 에러도
경고도 없이 **그 스타일만 조용히 사라진다.** 타입 검사도 eslint 도 못 잡는다(문자열이다).

실측 2026-09-17: 표 묶음 경계선을 `var(--border-strong)` 로 그렸는데 정의된 이름은
`--cims-border-strong` 이라 선이 아예 안 그려졌다. 번들에 코드는 들어가 있어서 배포·캐시를
두 번 의심한 뒤에야 찾았다. 같은 실수가 서비스·계측기 팩에도 3건 있었다.
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-CONSOLE-CSS-TOKENS"
_NAME = "콘솔 CSS 변수 정의 확인 (tests/test_console_css_tokens.py)"
_TEST = "tests/test_console_css_tokens.py"


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=21,
)
def console_css_tokens(ctx: VerifyContext) -> ItemResult:
    if not os.path.isfile(os.path.join(ctx.repo_root, _TEST)):
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail=f"{_TEST} 없음", stage=1)
    proc = subprocess.run(["python3", _TEST], cwd=ctx.repo_root,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    out = proc.stdout.decode("utf-8", "replace").strip()
    tail = "\n".join(out.splitlines()[-25:])
    ctx.w(f"## {_ID} — {_NAME}")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME,
                      status=ItemStatus.PASS if proc.returncode == 0 else ItemStatus.FAIL,
                      detail=tail[-400:], stage=1)
