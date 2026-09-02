"""S3 대표번호 병렬 호출(Flexible Alerting, TS 24.239) 회귀 — dispatch_center.md §4·§9.

관제 그룹의 대표번호(pilot)로 INVITE 하면 CSP TAS 가 등록 그룹원 전원에게 포크하고, 최초 200 OK 가 이겨
A 와 연결되며(RELAY_MODIFY peer1) 나머지는 CANCEL 된다. 무응답이면 `no_answer_sec` 뒤 `overflow_target`
으로 1단계 재시도, 없으면 480.

픽스처: 같은 org VOIP 가입자 4명(A=발신, B·C=그룹원, D=overflow 내선)으로 관제 그룹 `dg-vfy-<org>`
(pilot `7<org 뒷 3자리>…`, no_answer_sec=8, overflow=D) 를 **DB 에 직접 시드**하고 CSP 에
DISPATCH_GROUP_CHANGED 를 보낸다(멤버 pickup_group 도 그룹 id 로 파생, 종료 시 자기복원).
`dispatch_groups` 테이블 미적용 DB(migrate_dispatch_groups.sql) 면 SKIP.

검사 (판정 정본 = 4단말 누적 수신 RTP delta + A 의 최종 응답 `hunt_status` + 그룹원별 `*_invites`):
  F1 병렬 호출·응답 — A→pilot, B(ring-hold)·C 링, C 응답 → A·C 미디어, B 무흐름, B_invites=C_invites=1,
                     P-Called-Party-ID 에 대표번호
  F3 무응답 → overflow — B·C 전원 ring-hold → no_answer_sec 뒤 D 로 재시도, D 응답 → A·D 미디어, B·C 무흐름
  F5 대표번호 지정 픽업 — 그룹원 B·C·D 전원 ring-hold, D 가 `**<pilot>` 다이얼 → D 가 받음(포크 집합 재키잉·
                     RELAY_MODIFY), A·D 미디어, B·C 무흐름, pickup_status=200
  F6 sequential alerting(TS 24.239) — alert_mode=sequential, no_answer_sec=4: B 먼저 링(ring-hold) → 단계 시한 뒤
                     CANCEL → C 링·응답. hunt_status=200, C 승자, B_invites=C_invites=1, 응답 지연 ≥ 단계 시한
  F4 통화 중 제외(busy_members=skip)은 후속(SKIP 보고).
"""
from __future__ import annotations

import os
import re
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ...common import db as _db
from ...common.csp_notify import notify_csp_event
from ._xfer_common import (
    select_same_org, trio_cred_args, parse_marker_int,
    VOLTE_DOMAIN, VOLTE_TABLE, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks, notify_user_changed,
)

_RID = "S3-SCN-FA"
_RNAME = "대표번호 병렬 호출 (관제 그룹 pilot — 포크·승자·CANCEL·무응답 overflow)"

_DELTA4_RE = re.compile(r"RTP recv delta over \d+s: A=\+(\d+) B=\+(\d+) C=\+(\d+) D=\+(\d+)")


def _parse_delta4(text: str):
    last = None
    for line in text.splitlines():
        m = _DELTA4_RE.search(line)
        if m:
            last = m
    return None if not last else tuple(int(last.group(i)) for i in range(1, 5))


def _parse_marker_str(text: str, key: str):
    last = None
    for m in re.finditer(rf"\b{re.escape(key)}=(\S+)", text):
        last = m.group(1)
    return last


def _has_table(db_cfg: dict, table: str) -> bool:
    conn = _db.connect(db_cfg)
    try:
        with conn.cursor() as cur:
            cur.execute("SHOW TABLES LIKE %s", (table,))
            return cur.fetchone() is not None
    finally:
        conn.close()


class DispatchGroupFixture:
    """관제 그룹 1개 시드(+멤버 pickup_group 파생) + 자기복원. 테이블 부재면 active=False."""

    def __init__(self, dist_dir: str, csp_ip: str, group_id: str, pilot: str, members: list, overflow: str,
                 no_answer_sec: int = 8, alert_mode: str = "parallel", ptt_listen: str = "none",
                 listen_visibility: str = "hidden"):
        self.db_cfg = _db.csp_db_config(dist_dir)
        self.csp_ip = csp_ip
        self.group_id = group_id
        self.pilot = pilot
        self.members = list(members)
        self.overflow = overflow
        self.no_answer_sec = no_answer_sec
        self.alert_mode = alert_mode
        self.ptt_listen = ptt_listen
        self.listen_visibility = listen_visibility
        self.active = False
        self.reason = ""
        self._orig_pickup: dict = {}
        self._orig_member: dict = {}

    def __enter__(self):
        try:
            if not _has_table(self.db_cfg, "dispatch_groups"):
                self.reason = "dispatch_groups 테이블 부재 (migrate_dispatch_groups.sql 미적용)"
                return self
        except Exception as e:
            self.reason = f"DB 확인 실패: {type(e).__name__}"
            return self
        conn = _db.connect(self.db_cfg)
        try:
            with conn.cursor() as cur:
                cur.execute("DELETE FROM dispatch_groups WHERE id=%s", (self.group_id,))
                cur.execute(
                    "INSERT INTO dispatch_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, "
                    "busy_members, overflow_target, monitor_scope, ptt_listen, listen_visibility) "
                    "VALUES (%s,%s,%s,%s,%s,%s,'skip',%s,'none',%s,%s)",
                    (self.group_id, f"verify {self.group_id}", self.pilot or None,
                     "volte" if self.pilot else None, self.alert_mode, self.no_answer_sec, self.overflow or None,
                     self.ptt_listen, self.listen_visibility))
                for i, user in enumerate(self.members):
                    cur.execute("SELECT group_id FROM dispatch_group_members WHERE user_id=%s", (user,))
                    r = cur.fetchone()
                    self._orig_member[user] = (r[0] if isinstance(r, tuple) else (r or {}).get("group_id")) if r else None
                    cur.execute("INSERT INTO dispatch_group_members (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                                "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                                (user, self.group_id, i))
                    cur.execute(f"SELECT pickup_group FROM {VOLTE_TABLE} WHERE id=%s", (user,))
                    r = cur.fetchone()
                    self._orig_pickup[user] = (r[0] if isinstance(r, tuple) else (r or {}).get("pickup_group")) if r else None
                    cur.execute(f"UPDATE {VOLTE_TABLE} SET pickup_group=%s WHERE id=%s", (self.group_id, user))
        finally:
            conn.close()
        notify_csp_event("DISPATCH_GROUP_CHANGED", uri=self.group_id, action="POST", ip=self.csp_ip)
        for user in self.members:
            notify_user_changed(self.csp_ip, user)
        self.active = True
        time.sleep(0.5)
        return self

    def __exit__(self, *exc):
        if not self.active:
            return False
        try:
            conn = _db.connect(self.db_cfg)
            try:
                with conn.cursor() as cur:
                    cur.execute("DELETE FROM dispatch_groups WHERE id=%s", (self.group_id,))  # 멤버 행 CASCADE
                    for user, grp in self._orig_member.items():
                        if grp:
                            cur.execute("INSERT IGNORE INTO dispatch_group_members (user_id, group_id) VALUES (%s,%s)",
                                        (user, grp))
                    for user, pg in self._orig_pickup.items():
                        cur.execute(f"UPDATE {VOLTE_TABLE} SET pickup_group=%s WHERE id=%s", (pg, user))
            finally:
                conn.close()
            notify_csp_event("DISPATCH_GROUP_CHANGED", uri=self.group_id, action="DELETE", ip=self.csp_ip)
            for user in self.members:
                notify_user_changed(self.csp_ip, user)
        except Exception:
            pass
        return False


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=600,
    execution_order=66,
)
def flexible_alerting(ctx: VerifyContext) -> ItemResult:
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
    group_id = f"dg-vfy-{org}"
    pilot = f"7{str(org)[-3:].zfill(3)}0"  # 가입 id(E.164 +…)와 겹치지 않는 짧은 내선형 대표번호
    ctx.w(f"- 단말 org={org} A={A['user']} B={B['user']} C={C['user']} D={D['user']} pilot={pilot} group={group_id}")

    def run(tag: str, noanswer: bool, pickup: bool = False) -> tuple:
        args = [
            "-mode", "volte", "-scenario", "hunt", "-count", "4",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args([A, B, C, D], tag), "-media_dir", media_dir, "-duration", "4", "-no_video",
            "-pilot", pilot,
        ]
        if noanswer:
            args += ["-hunt_noanswer"]
        if pickup:
            args += ["-hunt_pickup"]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=240, tail_lines=400)
        return rc, _parse_delta4(tail), parse_marker_int(tail, "hunt_status"), tail

    def dstr(d) -> str:
        return "RTP delta 미출력" if d is None else f"recv A=+{d[0]} B=+{d[1]} C=+{d[2]} D=+{d[3]}"

    checks = []
    with DispatchGroupFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot, [B["user"], C["user"]], D["user"]) as fx:
        if not fx.active:
            ctx.w(f"- [SKIP] {fx.reason}")
            return done(ItemStatus.SKIP, fx.reason)

        # ── F1: 병렬 호출 — B ring-hold, C 응답 ──
        rc, d, st, tail = run("fa_f1", noanswer=False)
        b_inv, c_inv = parse_marker_int(tail, "B_invites"), parse_marker_int(tail, "C_invites")
        pcpid = _parse_marker_str(tail, "pcpid") or "-"
        ok = (d is not None and st == 200 and d[0] >= FLOW_MIN and d[2] >= FLOW_MIN and d[1] <= DROP_MAX
              and b_inv == 1 and c_inv == 1 and pilot in pcpid)
        checks.append(("F1 병렬 호출·응답 (C 승자, B CANCEL)", ok,
                       f"hunt_status={st} {dstr(d)} B_invites={b_inv} C_invites={c_inv} pcpid={pcpid} rc={rc}"))

        # ── F3: 무응답 → overflow(D) ──
        rc, d, st, tail = run("fa_f3", noanswer=True)
        d_inv = parse_marker_int(tail, "D_invites")
        ok = (d is not None and st == 200 and d[0] >= FLOW_MIN and d[3] >= FLOW_MIN
              and d[1] <= DROP_MAX and d[2] <= DROP_MAX and d_inv == 1)
        checks.append(("F3 무응답 → overflow 내선(D) 응답", ok,
                       f"hunt_status={st} {dstr(d)} D_invites={d_inv} (no_answer_sec={fx.no_answer_sec}) rc={rc}"))

        checks.append(("F4 통화 중 그룹원 제외(busy_members=skip)", None, "후속 — cspsim 사전 통화 구성 필요"))

    # ── F5: 대표번호 링잉 호 지정 픽업 — B·C·D 그룹원 전원 ring-hold, D 가 **<pilot> ──
    with DispatchGroupFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot, [B["user"], C["user"], D["user"]], "") as fx5:
        if fx5.active:
            rc, d, st, tail = run("fa_f5", noanswer=False, pickup=True)
            pk = parse_marker_int(tail, "pickup_status")
            by = _parse_marker_str(tail, "answered_by") or "-"
            ok = (d is not None and st == 200 and pk == 200 and by == D["user"] and d[0] >= FLOW_MIN
                  and d[3] >= FLOW_MIN and d[1] <= DROP_MAX and d[2] <= DROP_MAX)
            checks.append(("F5 대표번호 링잉 호 지정 픽업 (D **pilot → D 승계, B·C CANCEL)", ok,
                           f"hunt_status={st} pickup_status={pk} answered_by={by} {dstr(d)} rc={rc}"))

    # ── F6: sequential alerting — B(순번 0, ring-hold) 단계 시한 뒤 C(순번 1) 링·응답 ──
    seq_step = 4
    with DispatchGroupFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot, [B["user"], C["user"]], "",
                              no_answer_sec=seq_step, alert_mode="sequential") as fx6:
        if fx6.active:
            rc, d, st, tail = run("fa_f6", noanswer=False)
            b_inv, c_inv = parse_marker_int(tail, "B_invites"), parse_marker_int(tail, "C_invites")
            t_ans = parse_marker_int(tail, "t_answer_ms")
            by = _parse_marker_str(tail, "answered_by") or "-"
            ok = (d is not None and st == 200 and by == C["user"] and b_inv == 1 and c_inv == 1
                  and t_ans is not None and t_ans >= seq_step * 1000
                  and d[0] >= FLOW_MIN and d[2] >= FLOW_MIN and d[1] <= DROP_MAX)
            checks.append(("F6 sequential alerting (B 단계 시한 → C 응답, TS 24.239)", ok,
                           f"hunt_status={st} answered_by={by} B_invites={b_inv} C_invites={c_inv} "
                           f"t_answer_ms={t_ans} (≥{seq_step * 1000}) {dstr(d)} rc={rc}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
