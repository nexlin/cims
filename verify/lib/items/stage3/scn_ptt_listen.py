"""S3 PTT 그룹콜 청취(관제사 recvonly 합류) 회귀 — dispatch_center.md §5.6·§9.

멤버(A,B)가 그룹콜 중일 때 관제사 M 이 그룹 AoR 로 `a=recvonly` INVITE 하면 CSP 가 2단 인가
(자격 `ptt_user_profile.allow_ambient_listening` + 범위 관제 그룹 `ptt_listen`) 뒤 청취 멤버로 합류시킨다
(CMP `PTT_JOIN recv_only=1` — 상향 미중계·floor 요청 DENY, 로스터 은닉). 비멤버의 일반 INVITE 는 403.

픽스처(자기복원): M 을 멤버로 하는 관제 그룹 `dg-vfy-lsn-…`(ptt_listen/listen_visibility 를 검사별로 시드) +
`ptt_user_profile` M 행의 allow_ambient_listening. M 은 대상 PTT 그룹의 **비멤버** PTT 가입자를 고른다
(없으면 S3-SEED 창의 마지막 멤버). `dispatch_groups` 테이블·컬럼 미적용 DB 면 SKIP.

검사 (cspsim `ptt_listen` 결과 마커):
  L1 청취 합류 — allow=1·ptt_listen=all·hidden: join 200, M 수신 RTP delta>0, floor 요청 DENY(GRANT 0), 멤버 로스터에 M 없음,
     합류 전 M 의 conference SUBSCRIBE 200 (TS 24.379 §10.1.3.4.1 — 청취 범위를 <on-network-allow-conference-state> 해석으로 인가)
  L2 자격 없음 — allow_ambient_listening=0 → 403, M 무수신, conference SUBSCRIBE 403 + Warning 138
  L3 범위 밖 — ptt_listen=none → 403, conference SUBSCRIBE 403 + Warning 138
  L4 비멤버 일반 INVITE(sendrecv) → 403 (TS 24.379 비멤버 거절)
  L5 공개 청취 — listen_visibility=visible: join 200, 멤버 로스터에 M 노출(roles listener)
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
from ...common.subscribers import MCPTT_DOMAIN
from ._xfer_common import trio_cred_args, parse_marker_int, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks
from .scn_fa import DispatchGroupFixture

_RID = "S3-SCN-PTT-LISTEN"
_RNAME = "PTT 그룹콜 청취 (관제사 recvonly 합류 — 자격·범위 인가, floor DENY, 로스터 은닉/공개)"

_RES_RE = re.compile(r"PTT_LISTEN result: join_status=(-?\d+) members_in=(\d+) M_recv=\+(\d+) A_recv=\+(\d+) "
                     r"M_grant=(\d+) M_deny=(\d+) M_taken=(\d+) hidden=(\d+)(?: M_conf_sub=(-?\d+) M_conf_warn=(\d+))?")


def _parse_res(text: str):
    last = None
    for line in text.splitlines():
        m = _RES_RE.search(line)
        if m:
            last = m
    if not last:
        return None
    r = {k: int(last.group(i + 1)) for i, k in enumerate(("join", "members", "m_recv", "a_recv", "grant", "deny", "taken", "hidden"))}
    # conference 구독 마커 — 구 cspsim 은 미출력(-1 = 판정 불가)
    r["conf_sub"] = int(last.group(9)) if last.group(9) is not None else -1
    r["conf_warn"] = int(last.group(10)) if last.group(10) is not None else -1
    return r


def _pick_listener(db_cfg: dict, ptt_group: str, fallback: dict) -> dict:
    """대상 PTT 그룹의 비멤버 PTT 가입자 1명(UDP/digest·ha1 보유). 없으면 fallback(멤버)."""
    try:
        conn = _db.connect(db_cfg)
    except Exception:
        return fallback
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT s.id, s.imsi, COALESCE(s.ha1,'') FROM ptt_subscriptions s "
                "WHERE s.id LIKE '+%%' AND COALESCE(s.ha1,'')<>'' AND s.service_ref<>'' "
                "  AND COALESCE(s.sip_transport,'') IN ('','UDP') AND COALESCE(s.auth_scheme,'digest')='digest' "
                "  AND s.id NOT IN (SELECT m.user_id FROM ptt_group_members m JOIN ptt_groups g ON g.id=m.group_id "
                "                   WHERE g.mcptt_group_id=%s) "
                "ORDER BY s.id LIMIT 1", (ptt_group,))
            r = cur.fetchone()
            if not r:
                return fallback
            sid, imsi, ha1 = (r[0], r[1], r[2]) if isinstance(r, tuple) else (r["id"], r["imsi"], r["ha1"])
            return {"user": sid, "authId": f"{imsi}@{MCPTT_DOMAIN}" if imsi else "", "ha1": ha1}
    finally:
        conn.close()


class ListenerFixture:
    """관제사 M 시드 — 관제 그룹(멤버 M, ptt_listen/visibility) + ptt_user_profile.allow_ambient_listening. 자기복원."""

    def __init__(self, dist_dir: str, csp_ip: str, group_id: str, listener: str, allow: int,
                 ptt_listen: str, visibility: str = "hidden"):
        self.fx = DispatchGroupFixture(dist_dir, csp_ip, group_id, "", [listener], "", no_answer_sec=30,
                                       ptt_listen=ptt_listen, listen_visibility=visibility)
        self.listener = listener
        self.allow = allow
        self.active = False
        self.reason = ""
        self._orig = None  # (existed, prev_value)

    def __enter__(self):
        self.fx.__enter__()
        if not self.fx.active:
            self.reason = self.fx.reason
            return self
        conn = _db.connect(self.fx.db_cfg)
        try:
            with conn.cursor() as cur:
                cur.execute("SHOW COLUMNS FROM ptt_user_profile LIKE 'allow_ambient_listening'")
                if cur.fetchone() is None:
                    self.reason = "ptt_user_profile.allow_ambient_listening 컬럼 부재 (migrate_ptt_ambient_listening.sql 미적용)"
                    return self
                cur.execute("SELECT allow_ambient_listening FROM ptt_user_profile WHERE ptt_id=%s", (self.listener,))
                r = cur.fetchone()
                self._orig = (r is not None, (r[0] if isinstance(r, tuple) else r["allow_ambient_listening"]) if r else 0)
                cur.execute("INSERT INTO ptt_user_profile (ptt_id, allow_ambient_listening) VALUES (%s,%s) "
                            "ON DUPLICATE KEY UPDATE allow_ambient_listening=VALUES(allow_ambient_listening)",
                            (self.listener, self.allow))
        finally:
            conn.close()
        # 프로파일은 CSP 가 INVITE 시점에 DB 로 판정 — 캐시 없음. 그룹 맵은 DispatchGroupFixture 가 통지했다.
        self.active = True
        time.sleep(0.3)
        return self

    def __exit__(self, *exc):
        if self._orig is not None:
            try:
                conn = _db.connect(self.fx.db_cfg)
                try:
                    with conn.cursor() as cur:
                        existed, prev = self._orig
                        if existed:
                            cur.execute("UPDATE ptt_user_profile SET allow_ambient_listening=%s WHERE ptt_id=%s",
                                        (prev, self.listener))
                        else:
                            cur.execute("DELETE FROM ptt_user_profile WHERE ptt_id=%s", (self.listener,))
                finally:
                    conn.close()
            except Exception:
                pass
        return self.fx.__exit__(*exc)


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=900,
    execution_order=69,
)
def ptt_listen(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    s = ctx.state
    members = [c for c in (s.get("PTT_CREDS") or []) if c.get("ha1")]
    group = s.get("PTT_GROUP", "")
    if len(members) < 3 or not group:
        ctx.w("- [SKIP] S3-SEED PTT 자격 창(3명 이상)·그룹 미확보")
        return done(ItemStatus.SKIP, "PTT 자격 3명/그룹 미확보")
    A, B = members[0], members[1]
    M = _pick_listener(_db.csp_db_config(ctx.dist_dir), group, members[-1])
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    dg = f"dg-vfy-lsn-{group}"
    ctx.w(f"- 그룹={group} 멤버 A={A['user']} B={B['user']} 청취자 M={M['user']}"
          f"{' (그룹 멤버 — 비멤버 가입자 없음)' if M['user'] == members[-1]['user'] else ' (비멤버)'}")

    def run(tag: str, sendrecv: bool = False):
        args = [
            "-mode", "ptt", "-scenario", "ptt_listen", "-count", "3",
            "-ip", ctx.sim_ip, "-domain", s.get("PTT_DOM", MCPTT_DOMAIN),
            *trio_cred_args([A, B, M], tag), "-group", group, "-media_dir", media_dir, "-duration", "4",
        ]
        if sendrecv:
            args += ["-listen_sendrecv"]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=240, tail_lines=400)
        return rc, _parse_res(tail)

    def rstr(r) -> str:
        return "결과 마커 미출력" if r is None else (
            f"join_status={r['join']} members_in={r['members']} M_recv=+{r['m_recv']} A_recv=+{r['a_recv']} "
            f"M_grant={r['grant']} M_deny={r['deny']} M_taken={r['taken']} hidden={r['hidden']} "
            f"M_conf_sub={r['conf_sub']} M_conf_warn={r['conf_warn']}")

    checks = []
    # ── L1: 인가된 청취 (allow=1, ptt_listen=all, hidden) ──
    with ListenerFixture(ctx.dist_dir, ctx.sim_ip, dg, M["user"], 1, "all", "hidden") as fx:
        if not fx.active:
            ctx.w(f"- [SKIP] {fx.reason}")
            return done(ItemStatus.SKIP, fx.reason)
        rc, r = run("lsn_l1")
        ok = (r is not None and r["join"] == 200 and r["members"] >= 2 and r["m_recv"] >= FLOW_MIN
              and r["grant"] == 0 and r["deny"] >= 1 and r["hidden"] == 1)
        checks.append(("L1 청취 합류 (200, 수신 RTP, floor DENY, 로스터 은닉)", ok, f"{rstr(r)} rc={rc}"))
        checks.append(("L1b 범위 안 관제사 conference SUBSCRIBE → 200 (TS 24.379 §10.1.3.4.1)",
                       r is not None and r["conf_sub"] == 200, f"M_conf_sub={r['conf_sub'] if r else '-'}"))
        # ── L4: 비멤버 일반 INVITE → 403 ──
        if M["user"] != members[-1]["user"]:
            rc, r = run("lsn_l4", sendrecv=True)
            checks.append(("L4 비멤버 일반 INVITE(sendrecv) → 403", r is not None and r["join"] == 403,
                           f"{rstr(r)} rc={rc}"))
        else:
            checks.append(("L4 비멤버 일반 INVITE(sendrecv) → 403", None, "비멤버 PTT 가입자 없음 — 판정 불가"))
    # ── L2: 자격 없음 → 403 ──
    with ListenerFixture(ctx.dist_dir, ctx.sim_ip, dg, M["user"], 0, "all", "hidden") as fx:
        if fx.active:
            rc, r = run("lsn_l2")
            checks.append(("L2 자격 없음(allow_ambient_listening=0) → 403",
                           r is not None and r["join"] == 403 and r["m_recv"] <= DROP_MAX, f"{rstr(r)} rc={rc}"))
            checks.append(("L2b 자격 없음 conference SUBSCRIBE → 403 + Warning 138",
                           r is not None and r["conf_sub"] == 403 and r["conf_warn"] == 138,
                           f"M_conf_sub={r['conf_sub'] if r else '-'} warn={r['conf_warn'] if r else '-'}"))
    # ── L3: 범위 밖 → 403 ──
    with ListenerFixture(ctx.dist_dir, ctx.sim_ip, dg, M["user"], 1, "none", "hidden") as fx:
        if fx.active:
            rc, r = run("lsn_l3")
            checks.append(("L3 범위 밖(ptt_listen=none) → 403", r is not None and r["join"] == 403,
                           f"{rstr(r)} rc={rc}"))
            checks.append(("L3b 범위 밖 conference SUBSCRIBE → 403 + Warning 138",
                           r is not None and r["conf_sub"] == 403 and r["conf_warn"] == 138,
                           f"M_conf_sub={r['conf_sub'] if r else '-'} warn={r['conf_warn'] if r else '-'}"))
    # ── L5: 공개 청취 — 로스터 노출 ──
    with ListenerFixture(ctx.dist_dir, ctx.sim_ip, dg, M["user"], 1, "all", "visible") as fx:
        if fx.active:
            rc, r = run("lsn_l5")
            checks.append(("L5 공개 청취(listen_visibility=visible) — 로스터 노출",
                           r is not None and r["join"] == 200 and r["hidden"] == 0, f"{rstr(r)} rc={rc}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
