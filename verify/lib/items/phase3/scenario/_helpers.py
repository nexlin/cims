"""Phase 3 시나리오 공통 helper — cspsim 실행 + 녹취 delta 판정."""
from __future__ import annotations

import time

from ....registry import ItemResult, ItemStatus
from ....context import VerifyContext
from ....common.cspsim import run_cspsim
from ....common.recordings import count_recordings


def run_scenario(ctx: VerifyContext, item_id: str, title: str,
                 sim_args: list, prereq_keys: list,
                 timeout: int = 120) -> ItemResult:
    missing = [k for k in prereq_keys if not ctx.state.get(k)]
    if missing:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail=f"가입자/그룹 미준비: {','.join(missing)}", phase=3,
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
    mark = "[PASS]" if ok else "[FAIL]"
    ctx.w(f"- {mark} 녹취 +{delta} (rc={rc})")
    ctx.w()
    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"녹취 +{delta} (rc={rc})\n{tail[-500:]}", phase=3,
    )
