"""S1-UNIT-CONSOLE-STATS — 통계 화면이 값을 **어떻게 읽는지**를 지키는 콘솔 단위시험.

셋 다 "없는 값을 0 으로 읽지 않는다" 는 같은 규약을 각자의 자리에서 지킨다
(sip_statistics.md §2.1a·§2.1a-1·§2.1c):

  · tests/frontend/stats_no_value.test.mjs  null≠0(비율 빈칸) · 칸 숫자 = 툴팁 상세 합 ·
                                            자료 없는 날 행/합계는 빈칸
  · tests/frontend/matrix_fold.test.mjs     빈 구간 접기 경계 · 범위 표기
  · tests/frontend/source_notice.test.mjs   못 본 구간 경고 띠 — 문구·조회 조건별 분리·해제
  · tests/frontend/msg_key_order.test.mjs   메시지 교차표 열 순서 — 메서드 묶음·요청→응답·코드 수순
  · tests/frontend/cmp_series.test.mjs      CMP 시간대별 차트 계열 묶음 — 일반통화/PTT/배경 3축

TS 를 그대로 돌릴 수 없어 esbuild 로 번들해 node 로 실행한다. 콘솔 node_modules 가 없는
환경(빌드 안 한 트리)에서는 SKIP — 검사할 수 없는 것과 실패를 구분한다.
"""
from __future__ import annotations

import os
import tempfile

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell

_ID = "S1-UNIT-CONSOLE-STATS"
_NAME = "콘솔 통계 표시 규약 unit test (tests/frontend/*.test.mjs)"

# (시험 파일, 번들 대상 소스)
_CASES = [
    ("tests/frontend/stats_no_value.test.mjs",
     "ems/core/console/src/widgets/shapes/dataSourceSpec.ts"),
    ("tests/frontend/matrix_fold.test.mjs",
     "ems/core/console/src/widgets/shapes/matrixFold.ts"),
    ("tests/frontend/source_notice.test.mjs",
     "ems/core/console/src/widgets/shapes/sourceNotice.ts"),
    ("tests/frontend/msg_key_order.test.mjs",
     "ems/core/console/src/widgets/shapes/dataSourceSpec.ts"),
    ("tests/frontend/cmp_series.test.mjs",
     "ems/core/console/src/widgets/shapes/dataSourceSpec.ts"),
]


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=180,
    execution_order=55,
)
def unit_console_stats(ctx: VerifyContext) -> ItemResult:
    console_dir = os.path.join(ctx.repo_root, "ems", "core", "console")
    if not os.path.isdir(os.path.join(console_dir, "node_modules")):
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="ems/core/console/node_modules 없음 (콘솔 빌드 선행 필요)", stage=1)
    cases = [(t, s) for t, s in _CASES
             if os.path.isfile(os.path.join(ctx.repo_root, t))
             and os.path.isfile(os.path.join(ctx.repo_root, s))]
    if not cases:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="시험 파일 없음", stage=1)

    ctx.w(f"## {_ID} — {_NAME}")
    ok, tails = True, []
    with tempfile.TemporaryDirectory(prefix="console_unit_") as tmp:
        for test, src in cases:
            out_js = os.path.join(tmp, os.path.basename(src) + ".mjs")
            rc, o, e = shell.run(
                ["npx", "esbuild", src, "--bundle", "--format=esm",
                 "--platform=node", f"--outfile={out_js}"],
                cwd=ctx.repo_root, timeout=120)
            if rc != 0:
                ok = False
                tails.append(f"{os.path.basename(src)} 번들 실패: {(o + e).strip()[-300:]}")
                continue
            rc, o, e = shell.run(["node", test, out_js], cwd=ctx.repo_root, timeout=120)
            full = (o + e).strip()
            tail = "\n".join(full.splitlines()[-3:])
            tails.append(f"{os.path.basename(test)}: {tail}")
            ctx.w("```")
            for line in full.splitlines()[-25:]:
                ctx.w(line)
            ctx.w("```")
            if rc != 0:
                ok = False
    ctx.w()
    return ItemResult(id=_ID, name=_NAME,
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail=" · ".join(tails), stage=1)
