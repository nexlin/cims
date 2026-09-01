"""S3 당겨받기(call pickup) 회귀 — 링잉 호를 픽업 단말로 재고정 (volte_supplementary_services.md §5).

A→B 가 링잉(B 는 응답 보류) 중 C 가 당겨받기 코드('**')를 다이얼하면, 서버 PickUp 이 B 의
링잉 leg 를 회수(CANCEL)하고 그 자리를 C 로 재키잉해 A–C 를 연결한다(RELAY_MODIFY peer1).
검증 정본은 재고정 후 누적 수신 RTP delta:
  P1 그룹 픽업 — C 가 코드 다이얼 후 A·C 로 미디어가 흐르고 B(응답 보류)는 미디어 없음

A,B,C 는 같은 org(=픽업 그룹, dev 는 pickup_group 컬럼 부재 시 org 폴백). 전역 픽업 코드
`Setup.Sip.CallPickupId`(기본 '**') 또는 접속서비스 pickup_feature_code 와 일치해야 한다.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ._xfer_common import select_trio, trio_cred_args, parse_recv_delta, VOLTE_DOMAIN, FLOW_MIN, DROP_MAX

import os

_RID = "S3-SCN-PICKUP"
_RNAME = "당겨받기 (링잉 호 재고정 — pickup 그룹·RELAY_MODIFY)"


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=300,
    execution_order=64,
)
def pickup(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    creds, org = select_trio(ctx.dist_dir)
    if len(creds) < 3:
        ctx.w("- [SKIP] 같은 org(=픽업 그룹) VOIP 가입자 3명(A,B,C) 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 3명 미확보")
    cred_a = trio_cred_args(creds, "pickup")
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    ctx.w(f"- 단말 org={org} A={creds[0]['user']} B={creds[1]['user']} C={creds[2]['user']}")

    args = [
        "-mode", "volte", "-scenario", "pickup", "-count", "3",
        "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
        *cred_a, "-media_dir", media_dir, "-duration", "4", "-no_video",
        "-pickup_code", "**",
    ]
    rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)
    d = parse_recv_delta(tail)

    if d is None:
        ok = False
        detail = f"RTP delta 미출력(시나리오 미완) rc={rc}"
    else:
        a, b, c = d
        # A(발신)·C(픽업 단말) 로 미디어, B(응답 보류) 는 미디어 없음
        ok = a >= FLOW_MIN and c >= FLOW_MIN and b <= DROP_MAX
        detail = f"recv A=+{a} B=+{b} C=+{c} (A·C≥{FLOW_MIN}, B≤{DROP_MAX}) rc={rc}"

    ctx.w(f"- [{'PASS' if ok else 'FAIL'}] P1 그룹 픽업 — {detail}")
    return done(ItemStatus.PASS if ok else ItemStatus.FAIL, f"P1 그룹 픽업: {detail}")
