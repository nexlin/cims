"""S1-UE-FLOOR-CODEC — MCPTT floor 정의의 단일 정본 확인.

floor 메시지 정의는 `docs/design/features/mcptt_floor_defs.yaml` 하나가 정본이고, 서버(CMP)와
단말(코어)의 코드는 `scripts/gen_floor_defs.py` 가 거기서 낸다(ue_sdk.md §4.6). 손으로 고친 코드가
정본과 어긋나면 **단말과 서버가 다른 floor 를 말하게 된다** — TS 24.380 상호운용이 조용히 깨진다.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-UE-FLOOR-CODEC"
_NAME = "floor 정의 정본 일치 (gen_floor_defs --check)"


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=61,
)
def unit_ue_floor_codec(ctx: VerifyContext) -> ItemResult:
    gen = p(ctx.repo_root, "scripts", "gen_floor_defs.py")
    if not os.path.isfile(gen):
        return skip(_ID, _NAME, "scripts/gen_floor_defs.py 없음")
    rc, out, err = shell.run(["python3", gen, "--check"], cwd=ctx.repo_root, timeout=120)
    block(ctx, f"{_ID} — floor 정의 대조", (out + err).splitlines())
    if rc != 0:
        return done(_ID, _NAME, False,
                    "정본(mcptt_floor_defs.yaml)과 생성 코드가 어긋난다 — "
                    "`scripts/gen_floor_defs.py` 로 다시 낸다")
    return done(_ID, _NAME, True, "floor 정의 = 정본 일치")
