"""Phase 1 시나리오 공통 helper — 회귀 시나리오 _scenario() runner."""
from __future__ import annotations

import time

from ....registry import ItemResult, ItemStatus
from ....context import VerifyContext
from ....common.cspsim import run_cspsim
from ....common.recordings import count_recordings


def run_scenario(ctx: VerifyContext, item_id: str, title: str,
                 sim_args: list, prereq_keys: list,
                 timeout: int = 120) -> ItemResult:
    """cspsim 호출 → 시작 mtime 이후 녹취 +N 측정 → PASS/FAIL.

    prereq_keys: ctx.state 의 필수 키 (예: ["VOIP_USER"]) — 미존재 시 SKIP.
    """
    missing = [k for k in prereq_keys if not ctx.state.get(k)]
    if missing:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail=f"가입자/그룹 미준비: {','.join(missing)}", phase=1,
        )
    t0 = time.time()
    rc, tail = run_cspsim(ctx.repo_root, sim_args, timeout=timeout)
    delta = count_recordings(ctx.dist_dir, since=t0)
    ok = delta >= 1
    ctx.w(f"### {item_id} — {title}")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w(f"- {'[PASS]' if ok else '[FAIL]'} 녹취 +{delta} (rc={rc})")
    ctx.w()
    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"녹취 +{delta} (rc={rc})\n{tail[-500:]}", phase=1,
    )
