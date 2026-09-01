"""S3 dialog-event/당겨받기 BLF 회귀 (volte_supplementary_services.md §6.2) + SUBSCRIBE 분류.

관제 BLF 클릭 당겨받기의 표준 경로를 실측한다: C 가 B 를 dialog(RFC 4235) 구독 → A→B 링잉
시 서버가 dialog-info NOTIFY 로 링잉 leg Call-ID 를 전달 → C 가 그 Call-ID 로 INVITE-Replaces
(RFC 3891) → 서버가 원 relay 를 유지한 채 C 를 재고정해 A–C 연결. 두 표준(dialog-event 통지 +
Replaces 수신)이 한 흐름으로 엮인다. 그룹 축은 scn_pickup 과 같은 PickupGroupFixture.

검사:
  D1 dialog NOTIFY 수신 — C 가 dialog 구독 후 A→B 링잉 시 dialog-info NOTIFY 를 1건 이상 받는다
  D2 Replaces 재고정   — INVITE-Replaces 후 A·C 로 미디어가 흐르고(원 relay 승계) B(응답 보류)는 무흐름
  D3 그룹 밖 감시 403  — D(다른 pickup_group)의 B dialog 구독 → 403, NOTIFY 0건 (컬럼 축 한정)
  D4 미지 Event 489    — 미지원 이벤트 패키지 SUBSCRIBE → 489 Bad Event (RFC 6665 §8.2.1), 대조 dialog 자기감시 → 200
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ._xfer_common import (
    select_same_org, trio_cred_args, parse_recv_delta, parse_marker_int, PickupGroupFixture,
    VOLTE_DOMAIN, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks,
)

_RID = "S3-SCN-DIALOG"
_RNAME = "dialog-event/BLF 당겨받기 (RFC 4235 NOTIFY + RFC 3891 INVITE-Replaces) + SUBSCRIBE 분류(403/489)"

_BOGUS_EVENT = "cims-verify-bogus"


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=480,
    execution_order=65,
)
def dialog(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    creds, org = select_same_org(ctx.dist_dir, 4)
    if len(creds) < 4:
        ctx.w("- [SKIP] 같은 org VOIP 가입자 4명(A,B,C,D) 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 4명 미확보")
    A, B, C, D = creds
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    ctx.w(f"- 단말 org={org} A={A['user']} B={B['user']} C={C['user']} D={D['user']}")

    def run_blf(trio: list, tag: str) -> tuple:
        args = [
            "-mode", "volte", "-scenario", "dialog_pickup", "-count", "3",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args(trio, tag), "-media_dir", media_dir, "-duration", "4", "-no_video",
        ]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)
        return (rc, parse_marker_int(tail, "dialog_sub_status"), parse_marker_int(tail, "dialog_notify") or 0,
                parse_recv_delta(tail))

    def run_event(event: str, tag: str) -> tuple:
        args = [
            "-mode", "volte", "-scenario", "subscribe_event", "-count", "1",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args([A], tag), "-event", event,
        ]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=90)
        return rc, parse_marker_int(tail, "status")

    checks = []
    grp = f"vfy-pg-{org}"
    with PickupGroupFixture(ctx.dist_dir, ctx.sim_ip,
                            {A["user"]: grp, B["user"]: grp, C["user"]: grp, D["user"]: grp + "-x"}) as fx:
        ctx.w(f"- 그룹 축: {fx.axis}")

        # ── D1/D2: 같은 그룹 C 의 BLF 당겨받기 ──
        rc, sub_st, n_notify, d = run_blf([A, B, C], "dialog_d1")
        checks.append(("D1 dialog NOTIFY 수신", sub_st == 200 and n_notify >= 1,
                       f"dialog_sub_status={sub_st} dialog_notify={n_notify} rc={rc}"))
        if d is None:
            checks.append(("D2 Replaces 재고정", False, f"RTP delta 미출력(시나리오 미완) rc={rc}"))
        else:
            a, b, c = d
            ok = a >= FLOW_MIN and c >= FLOW_MIN and b <= DROP_MAX
            checks.append(("D2 Replaces 재고정", ok, f"recv A=+{a} B=+{b} C=+{c} (A·C≥{FLOW_MIN}, B≤{DROP_MAX})"))

        # ── D3: 다른 그룹 D 의 B 감시 → 403 ──
        if not fx.active:
            checks.append(("D3 그룹 밖 dialog 구독 403", None, "pickup_group 컬럼 부재 — org 폴백 축에서는 그룹 경계 검사 불가"))
        else:
            rc, sub_st, n_notify, d = run_blf([A, B, D], "dialog_d3")
            no_media = d is not None and d[2] <= DROP_MAX
            checks.append(("D3 그룹 밖 dialog 구독 403", sub_st == 403 and n_notify == 0 and no_media,
                           f"dialog_sub_status={sub_st} dialog_notify={n_notify} "
                           f"{'RTP delta 미출력' if d is None else f'recv D=+{d[2]}'} (기대 403·0건) rc={rc}"))

    # ── D4: 미지 Event → 489 (대조: dialog 자기감시 → 200) — 그룹 축 무관 ──
    rc_b, st_bogus = run_event(_BOGUS_EVENT, "dialog_d4b")
    rc_c, st_ctrl = run_event("dialog", "dialog_d4c")
    checks.append(("D4 미지 Event 489", st_bogus == 489 and st_ctrl == 200,
                   f"Event:{_BOGUS_EVENT} → {st_bogus} (기대 489) / 대조 Event:dialog 자기감시 → {st_ctrl} (기대 200) "
                   f"rc={rc_b}/{rc_c}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, f"axis={fx.axis}\n" + fmt_checks(checks))
