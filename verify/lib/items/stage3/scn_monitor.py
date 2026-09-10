"""S3 업무망 합법감청(통화 청취) 회귀 — dispatch_center.md §5·§9.

A↔B 통화 중 감청자 M 이 B 의 dialog 를 구독해 활성 호 call-id/태그를 학습한 뒤 INVITE-Join(RFC 3911,
recvonly)으로 청취 leg 에 합류한다. 서버가 CMP 청취 leg(tap)를 붙여 양 화자를 SSRC 2개로 분리 인도하고,
A/B 에게는 아무 변화가 없다(은닉). 인가는 둘이다(§5.2) — 규칙 1 같은 전화 그룹은 **BLF(dialog 구독)만** 열고,
Join(미디어 인도)은 감청자 회선의 **역할 `monitor_call`** 이 대상 당사자의 전화 그룹을 포함해야 한다(§5.3).

픽스처(자기복원, `_dispatch_common`): 감청 대상 전화 그룹 `pg-verify-a`(A·B 멤버) + 감시 역할 `role-verify-mon`
(monitor_call=all, M 의 person 에 배정) / 범위 밖 역할 `role-verify-out`(monitor_call=own, M' 은 `pg-verify-b` 멤버) /
역할 없는 그룹원 M'(`pg-verify-a` 또는 `pg-verify-b` 멤버). 테이블 미적용 DB 면 SKIP. 전환 전 스키마
(`dispatch_groups`)면 같은 의미를 관제 그룹으로 시드하되, "역할 없는 같은 그룹원의 Join 403" 은 신 스키마의 규칙이라
그 판정만 SKIP 으로 보고한다. CMP 가 resource.tap 을 광고하지 않으면 Join 이 488 → M2 FAIL 로 드러난다.

검사 (판정 정본 = 각 단말 누적 수신 RTP delta + 수신 SSRC 수 + Join 최종 응답 `join_status`. cspsim `monitor` 는
dialog 구독이 거절되면 Join 을 생략하고 그 응답을 join_status 로 내므로, 구독 응답은 진행 줄
`[BLF] dialog SUBSCRIBED OK` / `dialog SUBSCRIBE <n> 거절` 로, Join 시도 여부는 `M INVITE-Join(` 줄로 읽는다):
  M2  청취 — 역할 monitor_call=all 의 M: dialog 200 → Join 200, M 수신 RTP delta>0 & SSRC=2, A·B 수신 delta 무영향
  M5a 인가 — 범위 밖 역할(monitor_call=own, 타 그룹) M' → dialog 구독 403(Join 생략), 미디어 없음
  M5b 인가 — 전화 그룹원(A·B 와 같은 그룹)이지만 역할 없음 → 자기 그룹원 BLF dialog 200, Join 403, 미디어 없음
  M5c 인가 — 타 전화 그룹원·역할 없음 → dialog 구독 403
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
from ._dispatch_common import DispatchFixture, RoleSpec, SCHEMA_DISPATCH

_RID = "S3-SCN-MONITOR"
_RNAME = "업무망 합법감청 (INVITE-Join → CMP 청취 leg tap, SSRC 2개 분리 인도·은닉)"

_GRP_TARGET = "pg-verify-a"   # A·B (감청 대상)
_GRP_OTHER = "pg-verify-b"    # M' (타 그룹)
_ROLE_MON = "role-verify-mon"  # monitor_call=all
_ROLE_OUT = "role-verify-out"  # monitor_call=own (범위 밖)

_MONRE = re.compile(
    r"MONITOR result: join_status=(-?\d+) ab_ok=(\d+) M_recv=\+(\d+) M_ssrc=(\d+) A_recv=\+(\d+) B_recv=\+(\d+)")
_SUB_OK_RE = re.compile(r"\[BLF\] dialog SUBSCRIBED OK")
_SUB_REJ_RE = re.compile(r"MONITOR: dialog SUBSCRIBE (\d+) 거절")
_JOIN_RE = re.compile(r"MONITOR: M INVITE-Join\(")


def _parse_mon(text: str):
    last = None
    for m in _MONRE.finditer(text):
        last = m
    if not last:
        return None
    return tuple(int(last.group(i)) for i in range(1, 7))


class _Progress:
    """cspsim 진행 줄에서 M 의 dialog 구독 응답·Join 시도 여부를 모은다 (tail 잘림과 무관 — on_line)."""

    def __init__(self):
        self.dlg_sub = None       # 200 | 거절 코드 | None(응답 없음)
        self.join_tried = False

    def __call__(self, line: str) -> None:
        if _SUB_OK_RE.search(line):
            self.dlg_sub = 200
        m = _SUB_REJ_RE.search(line)
        if m:
            self.dlg_sub = int(m.group(1))
        if _JOIN_RE.search(line):
            self.join_tried = True


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
    ctx.w(f"- 단말 org={org} A={A['user']} B={B['user']} M={M['user']} M'={Mx['user']}")

    def fixture(group_id: str = "", members=(), role: RoleSpec | None = None, unassign=()) -> DispatchFixture:
        return DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, members=members, no_answer_sec=30, role=role,
                               unassign=unassign)

    def run(trio, tag):
        args = [
            "-mode", "volte", "-scenario", "monitor", "-count", "3",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args(trio, tag), "-media_dir", media_dir, "-duration", "5", "-no_video",
        ]
        prog = _Progress()
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=180, tail_lines=400, on_line=prog)
        return rc, _parse_mon(tail), parse_marker_int(tail, "join_status"), prog

    def mstr(d, st, prog) -> str:
        head = f"dlg_sub={prog.dlg_sub} join_tried={int(prog.join_tried)} join_status={st}"
        return head if d is None else (head + f" M_recv=+{d[2]} M_ssrc={d[3]} A_recv=+{d[4]} B_recv=+{d[5]}")

    checks = []
    with fixture(_GRP_TARGET, [A["user"], B["user"]]) as tgt:
        if not tgt.active:
            ctx.w(f"- [SKIP] {tgt.reason}")
            return done(ItemStatus.SKIP, tgt.reason)
        legacy = tgt.schema == SCHEMA_DISPATCH
        ctx.w(f"- 시드 스키마={tgt.schema} (대상 그룹 {_GRP_TARGET}: A·B)")

        # ── M2: 인가된 감청자(역할 monitor_call=all)가 A↔B 를 청취 ──
        with fixture(role=RoleSpec(_ROLE_MON, [M["user"]], monitor_call="all")) as fm:
            if not fm.active:
                checks.append(("M2 청취 (Join 200, M SSRC 2개, A·B 무영향)", False, f"역할 시드 실패 — {fm.reason}"))
            else:
                ctx.w(f"- M 역할 {_ROLE_MON}(monitor_call=all) person={fm.persons.get(M['user'], '-')}")
                rc, d, st, prog = run([A, B, M], "mon_m2")
                ok = (d is not None and st == 200 and d[2] >= FLOW_MIN and d[3] == 2 and
                      d[4] >= FLOW_MIN and d[5] >= FLOW_MIN)
                checks.append(("M2 청취 (Join 200, M SSRC 2개, A·B 무영향)", ok,
                               f"{mstr(d, st, prog)} (M·A·B≥{FLOW_MIN}, SSRC=2) rc={rc}"))

        # ── M5a: 범위 밖 역할 — M' 은 타 그룹(pg-verify-b) 멤버 + monitor_call=own → dialog 403, Join 생략 ──
        with fixture(_GRP_OTHER, [Mx["user"]], RoleSpec(_ROLE_OUT, [Mx["user"]], monitor_call="own")) as fo:
            if not fo.active:
                checks.append(("M5a 인가 — 범위 밖 역할(monitor_call=own·타 그룹) dialog 403", False,
                               f"역할 시드 실패 — {fo.reason}"))
            else:
                rc, d, st, prog = run([A, B, Mx], "mon_m5a")
                no_media = d is not None and d[2] <= DROP_MAX
                checks.append(("M5a 인가 — 범위 밖 역할(monitor_call=own·타 그룹) dialog 403",
                               prog.dlg_sub == 403 and not prog.join_tried and st == 403 and no_media,
                               f"{mstr(d, st, prog)} (기대 dlg_sub=403, Join 생략, M≤{DROP_MAX}) rc={rc}"))

        # ── M5c: 타 전화 그룹원·역할 없음 → dialog 403 ──
        with fixture(_GRP_OTHER, [Mx["user"]], unassign=[Mx["user"]]) as fc:
            if fc.active:
                rc, d, st, prog = run([A, B, Mx], "mon_m5c")
                no_media = d is not None and d[2] <= DROP_MAX
                checks.append(("M5c 인가 — 타 전화 그룹원·역할 없음 dialog 403",
                               prog.dlg_sub == 403 and not prog.join_tried and st == 403 and no_media,
                               f"{mstr(d, st, prog)} (기대 dlg_sub=403, Join 생략) rc={rc}"))

    # ── M5b: 같은 전화 그룹원(A·B·M')·역할 없음 → BLF dialog 200(규칙 1), Join 403(역할 필요), 미디어 없음 ──
    #   전환 전 스키마의 CSP 는 같은 그룹이면 Join 도 열었으므로(규칙 1 = 그룹 범위) 신 스키마에서만 판정한다.
    name_b = "M5b 인가 — 같은 전화 그룹원·역할 없음: BLF dialog 200 + Join 403"
    if legacy:
        checks.append((name_b, None, "전환 전 스키마(dispatch_groups) — 역할 없는 그룹원의 Join 403 은 phone_groups/roles 규칙"))
    else:
        with fixture(_GRP_TARGET, [A["user"], B["user"], Mx["user"]], unassign=[Mx["user"]]) as fb:
            if fb.active:
                rc, d, st, prog = run([A, B, Mx], "mon_m5b")
                no_media = d is not None and d[2] <= DROP_MAX
                checks.append((name_b, prog.dlg_sub == 200 and prog.join_tried and st == 403 and no_media,
                               f"{mstr(d, st, prog)} (기대 dlg_sub=200, Join 시도 → 403, M≤{DROP_MAX}) rc={rc}"))
            else:
                checks.append((name_b, False, f"그룹 시드 실패 — {fb.reason}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
