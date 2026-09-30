"""S1-UE-MCVIDEO-TC-DEFS — MCVideo 전송 제어 정의의 단일 정본 확인.

전송 제어(TS 24.581 §9.2·§11·§12.1.2) 정의는 `docs/design/features/mcvideo_tc_defs.yaml` 하나가 정본이고, 단말 코어
(`sdk/core/src/mcvideo/tc_defs.h`)와 CMP(`cmp/PTransmissionDefs.h`)의 상수는 `scripts/gen_mcvideo_tc_defs.py` 가 거기서 낸다
(mcvideo_dev_plan.md §3 K5). 손으로 고친 생성물이 정본과 어긋나면 **단말과 서버가 다른 전송 제어를 말하게 된다**.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-UE-MCVIDEO-TC-DEFS"
_NAME = "MCVideo 전송 제어 정의 정본 일치 (gen_mcvideo_tc_defs --check)"


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=61,
)
def unit_ue_mcvideo_tc_defs(ctx: VerifyContext) -> ItemResult:
    gen = p(ctx.repo_root, "scripts", "gen_mcvideo_tc_defs.py")
    if not os.path.isfile(gen):
        return skip(_ID, _NAME, "scripts/gen_mcvideo_tc_defs.py 없음")
    rc, out, err = shell.run(["python3", gen, "--check"], cwd=ctx.repo_root, timeout=120)
    block(ctx, f"{_ID} — 전송 제어 정의 대조", (out + err).splitlines())
    if rc != 0:
        return done(_ID, _NAME, False,
                    "정본(mcvideo_tc_defs.yaml)과 생성 헤더(sdk·cmp)가 어긋난다 — "
                    "`scripts/gen_mcvideo_tc_defs.py` 로 다시 낸다")
    return done(_ID, _NAME, True, "전송 제어 정의 = 정본 일치")
