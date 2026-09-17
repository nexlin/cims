"""단말 SDK(S1-UE-*) 항목 공용 — 경로와 SKIP 판정.

빌드 산출물이 없으면 FAIL 이 아니라 SKIP 이다. S1 은 정적 검사 stage 라 개발 장비에 Android SDK 나
`build/` 가 없을 수 있고, 그것을 실패로 세면 «실패를 PASS 로 쓰지 않는다» 의 반대편 잘못
(있지도 않은 실패를 만드는 것)을 저지른다. 다만 **건너뛴 이유를 반드시 문구로 남긴다**.
"""
from __future__ import annotations

import os

from ...registry import ItemResult, ItemStatus


def p(repo_root: str, *parts: str) -> str:
    return os.path.join(repo_root, *parts)


def skip(item_id: str, name: str, why: str) -> ItemResult:
    return ItemResult(id=item_id, name=name, status=ItemStatus.SKIP, detail=why, stage=1)


def done(item_id: str, name: str, ok: bool, detail: str) -> ItemResult:
    return ItemResult(
        id=item_id, name=name,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, stage=1,
    )


def block(ctx, title: str, lines) -> None:
    """리포트에 코드블록 한 덩이. 꼬리 30줄만 — 로그로 리포트를 덮지 않는다."""
    ctx.w(f"## {title}")
    tail = [x for x in lines if x][-30:]
    if tail:
        ctx.w("```")
        for line in tail:
            ctx.w(line)
        ctx.w("```")
    ctx.w()
