"""S3 PTT 일제 통화(broadcast group call) 회귀 — mcptt_broadcast_group_call.md §6.

일제 통화는 그룹 종류가 아니라 호 속성이다(TS 24.379 §4.12): 멤버가 편성 그룹 INVITE 의 mcptt-info 에 `<broadcast-ind>true` 를
실어 개시하면 그 세션만 개시자 floor 독점이 된다(TS 24.380 §6.3.5.3.4 — 타 참가자 요청은 Deny #5 Receive only).

검사:
  BC1/BC2 일제 통화 개시(cspsim `-broadcast`, 4인) — 전원 합류, 개시자만 GRANT, 나머지 3명 DENY
  BC6     같은 그룹을 표식 없이 개시 — 일반 그룹 통화, 4명 전원 GRANT (그룹 종류는 그대로 prearranged)
  BC7     애드혹 일제 통화(cspsim `-adhoc -broadcast` — TS 24.379 §17.2.2.1.1 9)) — 참가자 목록 전원 합류, 개시자만 GRANT, 나머지 DENY

계측기 경로(CIMS_TESTER_URL 설정 시): `PTT-GROUP-CALL-BROADCAST` 가 BC1·BC2·BC3(나갔다 broadcast-ind 로 재합류한 멤버는 개시자가
아니다)·BC5(일제 통화 중 conference SUBSCRIBE 480 + Warning 105)를, `PTT-FLOOR-HANDOVER` 가 BC6 을 본다. 계측기 워커는 ad hoc 그룹 통화를
내지 않아 BC7 은 cspsim 경로에서만 본다.
BC4(T4 만료 → 서버 해제)는 그룹 hang-timer(기본 30초)를 기다려야 해서 이 항목에 넣지 않는다 — cspsim `-broadcast` 로 그룹
`hang_timer_sec` 를 줄여 확인한다(§7.8 수동 절차).
"""
from __future__ import annotations

import os
import re
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ...common.tester import tester_config, tester_check
from ...common.subscribers import MCPTT_DOMAIN, cred_args
from ._xfer_common import fmt_checks, emit_checks

_RID = "S3-SCN-PTT-BROADCAST"
_RNAME = "PTT 일제 통화 (broadcast-ind 개시 — 개시자 floor 독점·개시자 고정·일반 통화와 공존)"
_COUNT = 4

_GRANT_RE = re.compile(r"GRANT received")
_DENY_RE = re.compile(r"Received opcode=3 \(DENY\)")
_JOIN_RE = re.compile(r"(\d+)/(\d+) members in call")


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=600,
    execution_order=71,
)
def ptt_broadcast(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    if tester_config() is not None:
        cfg = tester_config()
        ctx.w(f"- 경로: 계측기 @ {cfg['url']} · 토폴로지 {cfg['topology']}")
        checks = [
            tester_check(ctx, "BC1·BC2·BC3·BC5 일제 통화 — 개시자 독점·재합류자 Deny·구독 480/105",
                         "PTT-GROUP-CALL-BROADCAST", f"{_RID}/BC", ht=3),
            tester_check(ctx, "BC6 같은 그룹 표식 없이 — 일반 그룹 통화(경합·넘기기)", "PTT-FLOOR-HANDOVER", f"{_RID}/BC6", ht=3),
        ]
        ok = emit_checks(ctx, checks)
        return done(ItemStatus.PASS if ok else ItemStatus.FAIL, fmt_checks(checks))

    s = ctx.state
    group = s.get("PTT_GROUP", "")
    if not group or not s.get("PTT_USER"):
        ctx.w("- [SKIP] S3-SEED PTT 자격·그룹 미확보")
        return done(ItemStatus.SKIP, "PTT 자격/그룹 미확보")

    def _adhoc_id(user: str) -> str:
        # ad hoc 세션 id — 단말(SDK)과 같은 꼴 adhoc-<번호>-<epoch>(mcptt_emergency_modes.md §6). 편성 그룹 id 에 참가자 목록만
        #   얹으면 CSP 는 편성 그룹 호로 다룬다(ad hoc 경로가 아니다)
        return f"adhoc-{''.join(c for c in user if c.isdigit())}-{int(time.time())}"

    def run(broadcast: bool, adhoc: bool = False):
        args = [
            "-mode", "ptt", "-scenario", "group_call", "-count", str(_COUNT), "-duration", "10", "-floor_hold", "1",
            "-ip", ctx.sim_ip, "-user", s.get("PTT_USER", ""), "-domain", s.get("PTT_DOM", MCPTT_DOMAIN),
            *cred_args(s, "PTT", _COUNT), "-group", _adhoc_id(s.get("PTT_USER", "")) if adhoc else group,
            "-media_dir", os.path.join(ctx.repo_root, "tests", "media"),
        ]
        if broadcast:
            args.append("-broadcast")
        if adhoc:
            args.append("-adhoc")
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=240, tail_lines=400)
        m = _JOIN_RE.findall(tail)
        joined = int(m[-1][0]) if m else 0
        return rc, joined, len(_GRANT_RE.findall(tail)), len(_DENY_RE.findall(tail))

    checks = []
    rc, joined, grants, denies = run(True)
    checks.append(("BC1/BC2 일제 통화 개시 — 개시자만 GRANT, 나머지 DENY #5",
                   rc == 0 and joined == _COUNT and grants == 1 and denies == _COUNT - 1,
                   f"rc={rc} joined={joined}/{_COUNT} grant={grants}(기대 1) deny={denies}(기대 {_COUNT - 1})"))
    rc, joined, grants, denies = run(False)
    checks.append(("BC6 같은 그룹 표식 없이 — 일반 그룹 통화, 전원 GRANT",
                   rc == 0 and joined == _COUNT and grants == _COUNT and denies == 0,
                   f"rc={rc} joined={joined}/{_COUNT} grant={grants}(기대 {_COUNT}) deny={denies}(기대 0)"))
    rc, joined, grants, denies = run(True, adhoc=True)
    checks.append(("BC7 애드혹 일제 통화 — 참가자 전원 합류, 개시자만 GRANT, 나머지 DENY #5",
                   rc == 0 and joined == _COUNT and grants == 1 and denies == _COUNT - 1,
                   f"rc={rc} joined={joined}/{_COUNT} grant={grants}(기대 1) deny={denies}(기대 {_COUNT - 1})"))
    ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if ok else ItemStatus.FAIL, fmt_checks(checks))
