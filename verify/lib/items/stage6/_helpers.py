"""Stage 6 시나리오 공통 helper — cspsim 실행 + 녹취 delta 판정."""
from __future__ import annotations

import time
from typing import Optional

from ...registry import ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ...common.recordings import count_recordings, count_ptt_events


def run_scenario(ctx: VerifyContext, item_id: str, title: str,
                 sim_args: list, prereq_keys: list,
                 timeout: int = 120,
                 state_prefix: Optional[str] = None) -> ItemResult:
    """cspsim 시나리오 실행 + 녹취/PTT events delta 판정.

    PASS 조건: `seg_*.rtp` 신규 ≥1 **또는** PTT `events.jsonl` 신규 ≥1.
    PTT events 는 cmp 의 RTP 녹취가 비활성인 환경 (MediaTypes 에 audio
    누락) 에서 시나리오 정상 진행을 검증하는 fallback.

    `state_prefix` 가 지정되면 t0/tail/rc 를 `ctx.state[<PREFIX>_T0]`,
    `[<PREFIX>_TAIL]`, `[<PREFIX>_RC]` 로 저장하여 후속 항목이 재사용 가능.
    """
    missing = [k for k in prereq_keys if not ctx.state.get(k)]
    if missing:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail=f"가입자/그룹 미준비: {','.join(missing)}", stage=6,
        )
    t0 = time.time()
    rc, tail = run_cspsim(ctx.repo_root, sim_args, timeout=timeout)
    if state_prefix:
        ctx.state[f"{state_prefix}_T0"] = t0
        ctx.state[f"{state_prefix}_TAIL"] = tail
        ctx.state[f"{state_prefix}_RC"] = rc
    delta = count_recordings(ctx.dist_dir, since=t0)
    ev_delta = count_ptt_events(ctx.dist_dir, since=t0)
    ok = (delta + ev_delta) >= 1
    ctx.w(f"### {item_id} — {title}")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    mark = "[PASS]" if ok else "[FAIL]"
    ctx.w(f"- {mark} 녹취 +{delta} / PTT events +{ev_delta} (rc={rc})")
    ctx.w()
    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"녹취 +{delta} / PTT events +{ev_delta} (rc={rc})\n{tail[-500:]}",
        stage=6,
    )
