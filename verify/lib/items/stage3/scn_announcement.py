"""S3-SCN-ANN — 실패 안내(announcements.md §3.1·§3.2) 회귀.

없는 번호로 건 호가 **183+SDP(early media) → 안내 재생 → 원래 최종 코드(404)** 로 끝나는지 cspsim 출력으로 판정한다.
  · `=> RTP(<relay ip>:<port>)` 줄 = 발신 단말이 183 의 SDP 로 early media 를 세웠다(CSP 가 만든 answer, a=sendrecv)
  · `CALL ENDED … status=404` = 안내 뒤 최종 코드가 그대로 나갔다(answer 모델로 바꾸지 않았다)
  · 두 줄 사이 간격 ≥ 안내 길이(ann_invalid_number 6.16 s → 5 s 하한) = 재생을 끝까지 들려줬다(CANCEL 이 아니라 call_duration 10 s)
정책이 none 이거나 CMP 가 resource.ann 을 광고하지 않으면 183 없이 곧장 404 가 온다 — 그 경우는 FAIL 이 아니라 SKIP(안내 비활성 배치)로 둔다.
"""
from __future__ import annotations

import re
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.subscribers import VOLTE_DOMAIN, cred_args
from ...common.cspsim import run_cspsim

_ID = "S3-SCN-ANN"
_NAME = "실패 안내 — 없는 번호 → 183 early media 안내 → 404 (announcements.md §3.2)"
_NOWHERE = "+820000009999"
_MIN_PLAY_S = 5.0


@verify_item(
    id=_ID,
    stage=3, category="시나리오",
    name=_NAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=120,
    execution_order=66,
)
def announcement(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    ctx.w(f"### {_ID} — {_NAME}")
    if not s.get("VOIP_USER"):
        ctx.w("- [SKIP] 가입자 정보 부족: VOIP_USER")
        ctx.w()
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="가입자 미준비: VOIP_USER", stage=3)
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "10", "-ip", ctx.sim_ip,
        "-user", s.get("VOIP_USER", ""),
        "-domain", s.get("VOIP_DOM", VOLTE_DOMAIN),
        *cred_args(s, "VOIP", 2),
        "-callee_override", _NOWHERE, "-no_video",
    ]
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    t_rtp = []
    t_end = []
    codes = []

    def on_line(line: str):
        now = time.time()
        if "=> RTP(" in line:
            t_rtp.append(now)
        m = re.search(r"CALL ENDED .*status=(\d+)", line)
        if m:
            t_end.append(now)
            codes.append(int(m.group(1)))

    rc, tail = run_cspsim(ctx.repo_root, args, timeout=100, on_line=on_line)
    ctx.w("```")
    for line in tail.splitlines()[-40:]:
        ctx.w(line)
    ctx.w("```")
    if codes and all(c == 404 for c in codes) and not t_rtp:
        ctx.w("- [SKIP] 183 early media 없이 404 — 안내 비활성 배치(정책 none 또는 CMP resource.ann 미광고)")
        ctx.w()
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="안내 비활성(183 없음) — 404 만", stage=3)
    ok_code = bool(codes) and all(c == 404 for c in codes)
    ok_early = len(t_rtp) >= len(codes) and len(t_rtp) > 0
    gap = (min(t_end) - min(t_rtp)) if (t_rtp and t_end) else 0.0
    ok_len = gap >= _MIN_PLAY_S
    ok = ok_code and ok_early and ok_len
    detail = f"final codes={codes} early_media_legs={len(t_rtp)} play_gap={gap:.1f}s(≥{_MIN_PLAY_S}) rc={rc}"
    ctx.w(f"- {'[PASS]' if ok else '[FAIL]'} {detail}")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.PASS if ok else ItemStatus.FAIL, detail=detail, stage=3)
