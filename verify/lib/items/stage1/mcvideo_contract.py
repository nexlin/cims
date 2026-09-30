"""S1-MCVIDEO-CONTRACT — MCVideo 계약 골든(K2·K3)의 규격 스키마 검증.

`tests/fixtures/mcvideo/` 의 설정 문서(그룹 문서·MCVideo user profile·service configuration·ue-init-config)와 SIP 골든 메시지의 XML
본문(mcvideo-info·pidf mcvideoPresInfo)을 TS 24.481·24.484·24.281 XSD 로 **엄격** 검증한다(mcvideo_dev_plan.md §3). 생성 쪽(CSC) 시험은
S1-UNIT-CSC 의 tests/test_csc_mcvideo.py 가 «생성 = 골든» 으로 본다 — 이 항목은 골든 자체가 규격에 맞는지다.
xmlschema 가 없으면 SKIP(검사기가 이유를 적는다 — `CIMS_PYLIB=<xmlschema 설치 경로>`).
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-MCVIDEO-CONTRACT"
_NAME = "MCVideo 계약 골든 규격 스키마 검증 (tests/mcvideo_fixture_check.py)"


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=62,
)
def mcvideo_contract(ctx: VerifyContext) -> ItemResult:
    chk = p(ctx.repo_root, "tests", "mcvideo_fixture_check.py")
    if not os.path.isfile(chk):
        return skip(_ID, _NAME, "tests/mcvideo_fixture_check.py 없음")
    rc, out, err = shell.run(["python3", chk], cwd=ctx.repo_root, timeout=120)
    lines = (out + err).splitlines()
    block(ctx, f"{_ID} — MCVideo 계약 골든 스키마 검증", lines)
    if rc == 0 and out.lstrip().startswith("SKIP"):
        return skip(_ID, _NAME, out.strip().splitlines()[0])
    if rc != 0:
        return done(_ID, _NAME, False, "골든 문서가 규격 스키마·규칙에 어긋난다 — 위 FAIL 항목")
    return done(_ID, _NAME, True, lines[-1] if lines else "PASS")
