"""S3 대표번호 병렬 호출(Flexible Alerting, TS 24.239) 회귀 — dispatch_center.md §4·§9.

관제 그룹의 대표번호(pilot)로 INVITE 하면 CSP TAS 가 등록 그룹원 전원에게 포크하고, 최초 200 OK 가 이겨
A 와 연결되며(RELAY_MODIFY peer1) 나머지는 CANCEL 된다. 무응답이면 `no_answer_sec` 뒤 `overflow_target`
으로 1단계 재시도, 없으면 480.

픽스처: 같은 org VOIP 가입자 4명(A=발신, B·C=그룹원, D=overflow 내선)으로 전화 그룹 `pg-verify-a`
(pilot `7<org 뒷 3자리>…`, no_answer_sec=8, overflow=D) 를 **DB 에 직접 시드**하고 CSP 에
PHONE_GROUP_CHANGED 를 보낸다(멤버 pickup_group 도 그룹 id 로 파생, 종료 시 자기복원 — `_dispatch_common`).
역할은 쓰지 않는다 — 대표번호 호출·지정 픽업·그룹원 BLF(F7) 는 전화 그룹 축(CanWatch 규칙 1)만으로 성립한다.
전환 전 스키마(`dispatch_groups`)면 같은 의미를 관제 그룹으로 시드하고, 둘 다 없는 DB 면 SKIP.

검사 (판정 정본 = 4단말 누적 수신 RTP delta + A 의 최종 응답 `hunt_status` + 그룹원별 `*_invites`):
  F1 병렬 호출·응답 — A→pilot, B(ring-hold)·C 링, C 응답 → A·C 미디어, B 무흐름, B_invites=C_invites=1,
                     P-Called-Party-ID 에 대표번호
  F3 무응답 → overflow — B·C 전원 ring-hold → no_answer_sec 뒤 D 로 재시도, D 응답 → A·D 미디어, B·C 무흐름
  F5 대표번호 지정 픽업 — 그룹원 B·C·D 전원 ring-hold, D 가 `**<pilot>` 다이얼 → D 가 받음(포크 집합 재키잉·
                     RELAY_MODIFY), A·D 미디어, B·C 무흐름, pickup_status=200
  F6 sequential alerting(TS 24.239) — alert_mode=sequential, no_answer_sec=4: B 먼저 링(ring-hold) → 단계 시한 뒤
                     CANCEL → C 링·응답. hunt_status=200, C 승자, B_invites=C_invites=1, 응답 지연 ≥ 단계 시한
  F7 dialog 이벤트 정합(RFC 4235) — 그룹원 B 가 대표번호·A·C 를 dialog 구독, C 응답 뒤 **A(발신자)가 먼저 BYE**:
     세 entity 각각 자기 dialog(id 불변)로 confirmed→terminated 1회, local=entity·remote=상대·direction 불변
     (대표번호 recipient/remote A · A initiator/remote C · C recipient/remote A), entity 별 version 단조 증가.
  F4 통화 중 제외(busy_members=skip)은 후속(SKIP 보고).

계측기 경로(CIMS_TESTER_URL 설정 시, test_instrument.md §9): F1/F3/F5/F6 는 `VOLTE-FA-{PARALLEL,OVERFLOW,PICKUP,SEQUENTIAL}` —
invite.to 가 역할 아닌 대표번호 리터럴(`${pilot}` 바인딩), 포크 착신은 `fork_alert_pct`, 승자 외 CANCEL 은 정상. 전화 그룹 픽스처는 계획
드라이런의 역할 신원(memberB·memberC·overflow·picker)에 입힌다. F7 도 계측기(VOLTE-FA-DIALOG-FORK — subscribe to 대표번호 리터럴 + check dialog_consistent);
cspsim 경로(-hunt_watch)는 계측기 미설정일 때.
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ...common.tester import tester_config, tester_check, tester_role_identities
from ._xfer_common import (
    select_same_org, trio_cred_args, parse_marker_int,
    VOLTE_DOMAIN, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks,
)
from ._dispatch_common import DispatchFixture

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


_DLG_RE = re.compile(r"\[BLF\] dlg watcher=(\S+) entity=(\S+) ver=(-?\d+) id=(\S+) dir=(\S+) state=(\S+) local=(\S+) remote=(\S+)")


def _parse_dlg_records(text: str, watcher: str) -> list:
    """cspsim '[BLF] dlg …' 줄 → dict 목록(도착 순). watcher 세션 것만."""
    out = []
    for m in _DLG_RE.finditer(text):
        if m.group(1) != watcher:
            continue
        out.append({"entity": m.group(2), "ver": int(m.group(3)), "id": m.group(4), "dir": m.group(5),
                    "state": m.group(6), "local": m.group(7), "remote": m.group(8)})
    return out


def _judge_dialog_entity(recs: list, entity: str, exp_dir: str, exp_remote: str, exp_states: list) -> tuple:
    """entity 한 개의 dialog NOTIFY 열을 판정 — (ok, 설명). exp_states = partial 로 기대하는 상태 열(도착 순, 중복 없음)."""
    mine = [r for r in recs if r["entity"] == entity]
    vers = [r["ver"] for r in mine]
    # version 단조 — 한 NOTIFY 에 dialog 여러 개면 같은 version 이 연속으로 찍히므로 연속 중복만 허용
    mono = all(b > a for a, b in zip(vers, vers[1:]) if b != a) and all(
        not (b == a and mine[i]["id"] == mine[i + 1]["id"]) for i, (a, b) in enumerate(zip(vers, vers[1:])))
    dlg = [r for r in mine if r["id"] != "-"]
    ids = sorted({r["id"] for r in dlg})
    states = [r["state"] for r in dlg]
    dirs = sorted({r["dir"] for r in dlg})
    locals_ = sorted({r["local"] for r in dlg})
    remotes = sorted({r["remote"] for r in dlg})
    ok = (mono and len(ids) == 1 and states == exp_states and dirs == [exp_dir] and locals_ == [entity]
          and remotes == [exp_remote])
    return ok, (f"{entity}: ver={vers} ids={len(ids)} states={states} dir={dirs} local={locals_} remote={remotes} "
                f"(기대 states={exp_states} dir={exp_dir} local={entity} remote={exp_remote}, version 단조)")


def _parse_marker_str(text: str, key: str):
    last = None
    for m in re.finditer(rf"\b{re.escape(key)}=(\S+)", text):
        last = m.group(1)
    return last


def _run_hunt(ctx: VerifyContext, creds4: list, pilot: str, tag: str, noanswer: bool, pickup: bool = False, watch: bool = False) -> tuple:
    """cspsim hunt 시나리오(A,B,C,D) 한 번 → (rc, RTP delta 4, hunt_status, tail)."""
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    args = [
        "-mode", "volte", "-scenario", "hunt", "-count", "4",
        "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
        *trio_cred_args(creds4, tag), "-media_dir", media_dir, "-duration", "4", "-no_video",
        "-pilot", pilot,
    ]
    if noanswer:
        args += ["-hunt_noanswer"]
    if pickup:
        args += ["-hunt_pickup"]
    if watch:
        args += ["-hunt_watch"]
    rc, tail = run_cspsim(ctx.repo_root, args, timeout=240, tail_lines=900 if watch else 400)
    return rc, _parse_delta4(tail), parse_marker_int(tail, "hunt_status"), tail


def _f7_cspsim(ctx: VerifyContext, creds4: list, pilot: str, group_id: str, checks: list) -> None:
    """F7 dialog 이벤트 정합 — 그룹원 B 가 pilot·A·C 를 감시, C 응답 뒤 A(발신자) 선종료(A-leg BYE). cspsim -hunt_watch 전용 검사.
    A 도 멤버로 넣어 B 가 A 를 감시할 수 있게 한다(발신자는 포크 대상에서 제외되므로 B·C 만 울린다)."""
    A, B, C, _D = creds4
    with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot,
                         members=[A["user"], B["user"], C["user"]]) as fx7:
        if not fx7.active:
            checks.append(("F7 dialog 이벤트 정합", False, f"전화 그룹 시드 실패 — {fx7.reason}"))
            return
        rc, d, st, tail = _run_hunt(ctx, creds4, pilot, "fa_f7", noanswer=False, watch=True)
        sub_ok = parse_marker_int(tail, "dlg_sub_ok")
        recs = _parse_dlg_records(tail, B["user"])
        ok_p, s_p = _judge_dialog_entity(recs, pilot, "recipient", A["user"], ["early", "confirmed", "terminated"])
        ok_a, s_a = _judge_dialog_entity(recs, A["user"], "initiator", C["user"], ["confirmed", "terminated"])
        ok_c, s_c = _judge_dialog_entity(recs, C["user"], "recipient", A["user"], ["confirmed", "terminated"])
        ok = st == 200 and sub_ok == 3 and ok_p and ok_a and ok_c
        checks.append(("F7 dialog 이벤트 정합 (A-leg BYE — entity/direction/remote 불변·terminated 1회·version 단조)", ok,
                       f"hunt_status={st} dlg_sub_ok={sub_ok}/3 notify={len(recs)} rc={rc}\n"
                       f"      · {s_p}\n      · {s_a}\n      · {s_c}"))


def _via_tester(ctx: VerifyContext, done) -> ItemResult:
    """F1/F3/F5/F6 를 계측기로 — 계획 드라이런의 역할 신원을 전화 그룹(대표번호 pilot)에 시드한 뒤 `VOLTE-FA-*` 를 `--bind pilot=` 로.
    포크 판정은 시나리오 기대치(fork_alert_pct 100 · 승자 200 · CANCEL 정상 · F6 srd_ms.min ≥ 단계 시한). F7 은 VOLTE-FA-DIALOG-FORK 의 check dialog_consistent."""
    cfg = tester_config()
    ctx.w(f"- 경로: 계측기 @ {cfg['url']} · 토폴로지 {cfg['topology']} (F5 는 cspsim)")
    checks = []
    def dstr(d) -> str:
        return "RTP delta 미출력" if d is None else f"recv A=+{d[0]} B=+{d[1]} C=+{d[2]} D=+{d[3]}"
    binds0 = {"pilot": "0", "pickup_code": "**"}   # 계획 드라이런용 — 실제 pilot 은 시드 뒤 정한다

    def ids(scenario: str) -> dict:
        r = tester_role_identities(ctx, scenario, ht=4, binds=binds0) or {}
        if not all(r.get(k) for k in r):
            raise RuntimeError(f"{scenario}: 계획에 빈 역할 창 — {r}")
        return {k: v[0] for k, v in r.items()}

    def pilot_of(member: str) -> str:
        # 가입 id(E.164 +…)와 겹치지 않는 짧은 내선형 대표번호 — 그룹원 번호 끝 3자리로 유일하게
        return f"7{member[-3:]}0"

    def check(name: str, scenario: str, label: str, pilot: str) -> tuple:
        return tester_check(ctx, name, scenario, f"{_RID}/{label}", ht=4, binds={"pilot": pilot, "pickup_code": "**"})

    try:
        r1 = ids("VOLTE-FA-PARALLEL")
    except Exception as e:
        return done(ItemStatus.FAIL, f"계측기 계획 실패: {e}")
    group_id = "pg-verify-tester-fa"
    pilot = pilot_of(r1["memberB"])
    ctx.w(f"- F1 신원 caller={r1['caller']} B={r1['memberB']} C={r1['memberC']} pilot={pilot} group={group_id}")
    with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[r1["memberB"], r1["memberC"]]) as fx:
        if not fx.active:
            ctx.w(f"- [SKIP] {fx.reason}")
            return done(ItemStatus.SKIP, fx.reason)
        ctx.w(f"- 시드 스키마={fx.schema} (전화 그룹 {group_id}, 역할 없음)")
        checks.append(check("F1 병렬 호출·응답 (C 승자, B CANCEL)", "VOLTE-FA-PARALLEL", "F1", pilot))

    # F3 무응답 → overflow(D) — no_answer_sec=8(기본), 그룹원 B·C 응답 없음 → D 로 재시도
    try:
        r3 = ids("VOLTE-FA-OVERFLOW")
        with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[r3["memberB"], r3["memberC"]],
                             overflow=r3["overflow"]) as fx3:
            if fx3.active:
                checks.append(check("F3 무응답 → overflow 내선(D) 응답", "VOLTE-FA-OVERFLOW", "F3", pilot))
            else:
                checks.append(("F3 무응답 → overflow 내선(D) 응답", False, f"시드 실패 — {fx3.reason}"))
    except Exception as e:
        checks.append(("F3 무응답 → overflow 내선(D) 응답", False, f"계측기 계획 실패: {e}"))

    checks.append(("F4 통화 중 그룹원 제외(busy_members=skip)", None, "후속 — 사전 통화 구성 필요"))

    # F6 sequential alerting — first 단계 시한(4 s) 뒤 second 링·응답, srd_ms.min ≥ 4000 은 시나리오 기대치
    try:
        r6 = ids("VOLTE-FA-SEQUENTIAL")
        with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[r6["first"], r6["second"]],
                             no_answer_sec=4, alert_mode="sequential") as fx6:
            if fx6.active:
                checks.append(check("F6 sequential alerting (first 단계 시한 → second 응답, TS 24.239)", "VOLTE-FA-SEQUENTIAL", "F6", pilot))
            else:
                checks.append(("F6 sequential alerting", False, f"시드 실패 — {fx6.reason}"))
    except Exception as e:
        checks.append(("F6 sequential alerting", False, f"계측기 계획 실패: {e}"))

    # F5(링잉 대표번호 지정 픽업 — PickUpFork 완결)·F7(dialog 포크 정합)은 cspsim 검사 — 계측기 누계 지표로는 픽업 승계·entity 별 NOTIFY 방향을
    #   가릴 수 없다(MONITOR 가 M2 만 계측기로 두는 것과 같은 이유). cspsim 헬퍼는 대상 CSP 의 SIP 접속점(dev 5060)이 필요하므로 계측기 모드
    #   (대개 배포 15060)에서는 생략하고 정보로만 남긴다 — dev CSP 5060 상대로 계측기 미설정(cspsim 경로)일 때 F1~F7 전부 판정된다(D5·M8·M5 와 같은 규약).
    checks.append(("F5 대표번호 링잉 호 지정 픽업 (PickUpFork)", None, "cspsim 경로 — dev CSP 5060 필요(계측기 미설정 시 판정). 계측기 모드에서는 생략"))
    # F7 dialog 포크 정합 — 계측기 VOLTE-FA-DIALOG-FORK: memberB 가 pilot·caller·memberC 를 dialog 구독, check dialog_consistent(entity 별 dialog 하나·
    #   local/remote/direction 불변·상태 전진·terminated 1회·version 단조 — cspsim _judge_dialog_entity 와 같은 규칙). caller 도 전화 그룹원(B 가 A 를 감시 — CanWatch 규칙 1)
    try:
        r7 = ids("VOLTE-FA-DIALOG-FORK")
        with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[r7["caller"], r7["memberB"], r7["memberC"]]) as fx7:
            if fx7.active:
                checks.append(check("F7 dialog 이벤트 포크 정합 (RFC 4235 — entity/direction/remote 불변·terminated 1회·version 단조)", "VOLTE-FA-DIALOG-FORK", "F7", pilot))
            else:
                checks.append(("F7 dialog 이벤트 포크 정합 (RFC 4235)", False, f"시드 실패 — {fx7.reason}"))
    except Exception as e:
        checks.append(("F7 dialog 이벤트 포크 정합 (RFC 4235)", False, f"계측기 계획 실패: {e}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))


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

    if tester_config() is not None:
        return _via_tester(ctx, done)

    creds, org = select_same_org(ctx.dist_dir, 4)
    if len(creds) < 4:
        ctx.w("- [SKIP] 같은 org VOIP 가입자 4명(A,B,C,D) 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 4명 미확보")
    A, B, C, D = creds
    group_id = "pg-verify-a"
    pilot = f"7{str(org)[-3:].zfill(3)}0"  # 가입 id(E.164 +…)와 겹치지 않는 짧은 내선형 대표번호
    ctx.w(f"- 단말 org={org} A={A['user']} B={B['user']} C={C['user']} D={D['user']} pilot={pilot} group={group_id}")

    def run(tag: str, noanswer: bool, pickup: bool = False, watch: bool = False) -> tuple:
        return _run_hunt(ctx, [A, B, C, D], pilot, tag, noanswer, pickup, watch)

    def dstr(d) -> str:
        return "RTP delta 미출력" if d is None else f"recv A=+{d[0]} B=+{d[1]} C=+{d[2]} D=+{d[3]}"

    checks = []
    with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[B["user"], C["user"]],
                         overflow=D["user"]) as fx:
        if not fx.active:
            ctx.w(f"- [SKIP] {fx.reason}")
            return done(ItemStatus.SKIP, fx.reason)
        ctx.w(f"- 시드 스키마={fx.schema} (전화 그룹 {group_id}, 역할 없음)")

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
    with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot,
                         members=[B["user"], C["user"], D["user"]]) as fx5:
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
    with DispatchFixture(ctx.dist_dir, ctx.sim_ip, group_id, pilot=pilot, members=[B["user"], C["user"]],
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

    # ── F7: dialog 이벤트 정합 — 그룹원 B 가 pilot·A·C 감시, C 응답 뒤 A(발신자) 선종료(A-leg BYE) ──
    _f7_cspsim(ctx, [A, B, C, D], pilot, group_id, checks)

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
