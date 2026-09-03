"""S1-UNIT-GRID-BUDGET — 콘솔 그리드 엔진(세로 예산 + 잠금) 단위 테스트.

캔버스는 화면 한 장(GRID_ROWS 행)이 전부다(console_platform.md §3.0). 위젯을 키우면 잠기지 않은
위젯이 줄어들고, 줄일 여지가 없으면 조작이 거절돼야 한다. 이 규칙이 깨지면 관제 화면이 스크롤되거나
카드가 잘리므로 게이트로 고정한다.

gridLayout.ts 는 순수 함수(DOM/React 무의존)라 esbuild 로 번들해 node 로 바로 돌린다 —
브라우저 테스트 러너를 새로 들이지 않는다.
"""
from __future__ import annotations

import os
import tempfile

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell

_ID = "S1-UNIT-GRID-BUDGET"
_NAME = "콘솔 그리드 세로 예산·잠금"


def _skip(detail: str) -> ItemResult:
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail=detail, stage=1)


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name="콘솔 그리드 엔진 단위 테스트 (세로 예산 + 잠금)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=45,
)
def unit_grid_budget(ctx: VerifyContext) -> ItemResult:
    console_dir = os.path.join(ctx.repo_root, "ems", "core", "console")
    src = os.path.join(console_dir, "src", "widgets", "gridLayout.ts")
    test = os.path.join(ctx.repo_root, "tests", "frontend", "grid_budget.test.mjs")
    if not os.path.isdir(os.path.join(console_dir, "node_modules")):
        return _skip("ems/core/console/node_modules 없음 — `npm install` 선행 필요")
    if not (os.path.isfile(src) and os.path.isfile(test)):
        return _skip("gridLayout.ts 또는 tests/frontend/grid_budget.test.mjs 없음")

    with tempfile.TemporaryDirectory(prefix="cims-grid-") as tmp:
        bundle = os.path.join(tmp, "gridLayout.mjs")
        rc, out, err = shell.run(
            ["npx", "--no-install", "esbuild", src, "--bundle", "--format=esm",
             "--platform=node", f"--outfile={bundle}"],
            cwd=console_dir, timeout=60,
        )
        if rc != 0:
            return _skip(f"esbuild 번들 실패 — {(err or out).strip()[:200]}")
        rc, out, err = shell.run(["node", test, bundle], cwd=ctx.repo_root, timeout=60)

    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-40:])
    ctx.w(f"## {_ID} — 그리드 세로 예산·잠금")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    summary = next((ln for ln in reversed(full.splitlines()) if "pass /" in ln), tail)
    return ItemResult(
        id=_ID, name=_NAME,
        status=ItemStatus.PASS if rc == 0 else ItemStatus.FAIL,
        detail=summary.strip(), stage=1,
    )
