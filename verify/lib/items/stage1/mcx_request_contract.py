"""S1-MCX-REQUEST-CONTRACT — MCPTT·MCData 요청 형식 골든의 최신성.

`tests/fixtures/mcptt/sip/` 의 규격형 요청 골든(개별 호 INVITE · SDS·FD MESSAGE · 미디어 평면 SDS INVITE · 거절 응답)은
`build_goldens.py` 가 정본이다 — `--check` 가 *.txt 가 그 출력과 바이트 단위로 같은지 본다(docs/dev/conformance_gap_plan.md §7
«요청 형식 계약»). 서버 해석은 S1-UNIT-CSP 의 tests/csp_mcptt_request_test.cpp, 단말 생성은 SDK 단위시험이 같은 파일을 읽는다.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-MCX-REQUEST-CONTRACT"
_NAME = "MCPTT·MCData 요청 형식 골든 최신성 (tests/fixtures/mcptt/sip/build_goldens.py --check)"


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=62,
)
def mcx_request_contract(ctx: VerifyContext) -> ItemResult:
    gen = p(ctx.repo_root, "tests", "fixtures", "mcptt", "sip", "build_goldens.py")
    if not os.path.isfile(gen):
        return skip(_ID, _NAME, "tests/fixtures/mcptt/sip/build_goldens.py 없음")
    rc, out, err = shell.run(["python3", gen, "--check"], cwd=ctx.repo_root, timeout=60)
    lines = (out + err).splitlines()
    block(ctx, f"{_ID} — 요청 형식 골든 최신성", lines)
    if rc != 0:
        return done(_ID, _NAME, False, "골든 *.txt 가 build_goldens.py 와 다르다 — 위 FAIL 항목")
    return done(_ID, _NAME, True, lines[-1] if lines else "PASS")
