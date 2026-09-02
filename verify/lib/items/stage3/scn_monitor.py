"""S3 업무망 합법감청(통화 청취) 회귀 — dispatch_center.md §5·§9.

A↔B 통화 중 감청자 M 이 B 의 dialog 를 구독해 활성 호 call-id/태그를 학습한 뒤 INVITE-Join(RFC 3911,
recvonly)으로 청취 leg 에 합류한다. 서버가 CMP 청취 leg(tap)를 붙여 양 화자를 SSRC 2개로 분리 인도하고,
A/B 에게는 아무 변화가 없다(은닉). 감청자 M 은 monitor_scope 로 대상 그룹을 포함하는 관제 그룹에 속해야 한다.

픽스처: 감청 대상 그룹 `dg-vfy-mon-a`(A·B 멤버)와 감청 그룹 `dg-vfy-mon-m`(M 멤버, monitor_scope=all)을
DB 에 시드(자기복원). dispatch_groups 테이블 미적용 DB 면 SKIP. CMP 가 resource.tap 을 광고하지 않으면
Join 이 488 → M2 FAIL 로 드러난다(청취 미지원 노드 명시).

검사 (판정 정본 = 각 단말 누적 수신 RTP delta + 수신 SSRC 수 + Join 최종 응답 `join_status`):
  M1/M2 청취 — M Join → join_status=200, M 수신 RTP delta>0 & SSRC=2, A·B 수신 delta 는 통화 그대로(무영향)
  M5 인가   — 범위 밖 감청자(관제 그룹 없음)의 Join → 403, 미디어 없음
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ._xfer_common import (
    select_same_org, trio_cred_args, parse_marker_int, VOLTE_DOMAIN, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks,
)
from .scn_fa import DispatchGroupFixture

_RID = "S3-SCN-MONITOR"
_RNAME = "업무망 합법감청 (INVITE-Join → CMP 청취 leg tap, SSRC 2개 분리 인도·은닉)"

_MONRE = re.compile(
    r"MONITOR result: join_status=(-?\d+) ab_ok=(\d+) M_recv=\+(\d+) M_ssrc=(\d+) A_recv=\+(\d+) B_recv=\+(\d+)")


def _parse_mon(text: str):
    last = None
    for m in _MONRE.finditer(text):
        last = m
    if not last:
        return None
    return tuple(int(last.group(i)) for i in range(1, 7))


class DispatchMemberFixture:
    """단일 관제 그룹(멤버·범위 지정) 시드 + 자기복원 — DispatchGroupFixture(대표번호 없음) 래핑."""

    def __init__(self, dist_dir, csp_ip, group_id, members, monitor_scope="none"):
        self.fx = DispatchGroupFixture(dist_dir, csp_ip, group_id, "", members, "", no_answer_sec=30)
        self._scope = monitor_scope
        self.active = False

    def __enter__(self):
        # DispatchGroupFixture 는 monitor_scope='none' 로 시드하므로, scope 를 UPDATE 로 덮는다.
        self.fx.__enter__()
        self.active = self.fx.active
        if self.active and self._scope != "none":
            import pymysql  # noqa
            from ...common import db as _db
            conn = _db.connect(self.fx.db_cfg)
            try:
                with conn.cursor() as cur:
                    cur.execute("UPDATE dispatch_groups SET monitor_scope=%s WHERE id=%s",
                                (self._scope, self.fx.group_id))
            finally:
                conn.close()
            from ...common.csp_notify import notify_csp_event
            notify_csp_event("DISPATCH_GROUP_CHANGED", uri=self.fx.group_id, action="PUT", ip=self.fx.csp_ip)
        return self

    def __exit__(self, *exc):
        return self.fx.__exit__(*exc)


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=600,
    execution_order=68,
)
def monitor(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    creds, org = select_same_org(ctx.dist_dir, 4)
    if len(creds) < 4:
        ctx.w("- [SKIP] 같은 org VOIP 가입자 4명(A,B,M,M') 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 4명 미확보")
    A, B, M, Mx = creds
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    grp_target = f"dg-vfy-mona-{org}"   # A·B (감청 대상)
    grp_mon = f"dg-vfy-monm-{org}"      # M (monitor_scope=all)
    ctx.w(f"- 단말 org={org} A={A['user']} B={B['user']} M={M['user']} M'={Mx['user']}")

    def run(trio, tag):
        args = [
            "-mode", "volte", "-scenario", "monitor", "-count", "3",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args(trio, tag), "-media_dir", media_dir, "-duration", "5", "-no_video",
        ]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)
        return rc, _parse_mon(tail), parse_marker_int(tail, "join_status")

    checks = []
    tgt = DispatchMemberFixture(ctx.dist_dir, ctx.sim_ip, grp_target, [A["user"], B["user"]], "none")
    with tgt:
        if not tgt.active:
            ctx.w("- [SKIP] dispatch_groups 테이블 부재 (migrate_dispatch_groups.sql 미적용)")
            return done(ItemStatus.SKIP, "dispatch_groups 테이블 부재")

        # ── M1/M2: 인가된 감청자(monitor_scope=all)가 A↔B 를 청취 ──
        with DispatchMemberFixture(ctx.dist_dir, ctx.sim_ip, grp_mon, [M["user"]], "all"):
            rc, d, st = run([A, B, M], "mon_m2")
            ok = (d is not None and st == 200 and d[2] >= FLOW_MIN and d[3] == 2 and
                  d[4] >= FLOW_MIN and d[5] >= FLOW_MIN)
            checks.append(("M2 청취 (Join 200, M SSRC 2개, A·B 무영향)", ok,
                           None if d is None else
                           f"join_status={st} M_recv=+{d[2]} M_ssrc={d[3]} A_recv=+{d[4]} B_recv=+{d[5]} "
                           f"(M·A·B≥{FLOW_MIN}, SSRC=2) rc={rc}"))

        # ── M5: 관제 그룹 없는 M'(범위 밖)의 Join → 403 ──
        rc, d, st = run([A, B, Mx], "mon_m5")
        no_media = d is not None and d[2] <= DROP_MAX
        checks.append(("M5 인가 — 범위 밖 감청자 Join 403", st == 403 and no_media,
                       None if d is None else f"join_status={st} M_recv=+{d[2]} (기대 403, M≤{DROP_MAX}) rc={rc}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
