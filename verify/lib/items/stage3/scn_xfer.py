"""S3 호 전달(REFER) 회귀 — blind/attended 전달의 미디어 재고정 + 전달 권한 게이트 (volte_supplementary_services.md §6).

A→B 통화를 세운 뒤 A 가 REFER 로 상대를 C 에게 넘긴다. 서버(B2BUA)가 REFER 를 종단하고
원 통화의 relay 세션을 유지한 채 교체되는 leg 만 RELAY_MODIFY 로 재고정한다(포트 산술 금지).
검증 정본은 전달 후 각 단말의 누적 수신 RTP delta(+ REFER 최종 응답 `refer_status`):
  X1 blind 전달    — A blind REFER(→C) 후 B·C 로 미디어가 흐르고(원 relay 승계) A(전달자)는 드롭
  X2 attended 전달 — A→B + A→C(상담) 후 attended REFER(Replaces): B·C 로 미디어, A 드롭
  X3 전달 권한 403 — A 의 service_ref 를 `transfer_allowed=false` 서비스(S3-SEED 시드 NOXFER_SERVICE_REF)로
                     플립(+USER_CHANGED) 후 blind REFER → 403, 원 통화 A–B 유지·C 무흐름. 종료 시 원값 복원.

cspsim 3 단말(A,B,C)은 같은 org(픽업 그룹 축 무관)이며 dev DB 가입자에서 고른다.

계측기 경로(test_instrument.md §9 — `CIMS_TESTER_URL`·`CIMS_TESTER_TOPOLOGY`): X1 → `VOLTE-XFER-BLIND`, X2 → `VOLTE-XFER-ATTENDED`,
X3 → 계획 드라이런으로 transferor 역할의 신원을 얻어 service_ref 를 플립한 뒤 `VOLTE-XFER-DENIED`(REFER 403 기대). 판정 = 계측기 verdict.
"""
from __future__ import annotations

import json
import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.cspsim import run_cspsim
from ...common.tester import tester_config, tester_check, tester_role_identities
from ...common.access_services import NOXFER_SERVICE_REF
from ...common.subscribers import get_service_ref, set_service_ref
from ._xfer_common import (
    select_trio, trio_cred_args, parse_recv_delta, parse_marker_int, notify_user_changed,
    VOLTE_DOMAIN, VOLTE_TABLE, FLOW_MIN, DROP_MAX, fmt_checks, emit_checks,
)

_RID = "S3-SCN-XFER"
_RNAME = "호 전달 (blind/attended REFER — 미디어 재고정·원 relay 승계·transfer_allowed 게이트)"


def _noxfer_seeded(dist_dir: str) -> bool:
    path = os.path.join(dist_dir, "config", "access_services.jsonl")
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                if rec.get("name") == NOXFER_SERVICE_REF and rec.get("enabled", True):
                    return True
    except Exception:
        pass
    return False


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=480,
    execution_order=63,
)
def xfer(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    if tester_config() is not None:
        return _via_tester(ctx, done)

    creds, org = select_trio(ctx.dist_dir)
    if len(creds) < 3:
        ctx.w("- [SKIP] 같은 org VOIP 가입자 3명(A,B,C) 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 3명 미확보")
    cred_a = trio_cred_args(creds, "xfer")
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    ctx.w(f"- 단말 org={org} A={creds[0]['user']} B={creds[1]['user']} C={creds[2]['user']}")

    def run(scenario: str) -> tuple:
        args = [
            "-mode", "volte", "-scenario", scenario, "-count", "3",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *cred_a, "-media_dir", media_dir, "-duration", "4", "-no_video",
        ]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)
        return rc, tail, parse_recv_delta(tail)

    checks = []  # (이름, ok|None, 상세)

    # ── X1: blind 전달 — B·C 미디어, A 드롭 ──
    rc1, t1, d1 = run("transfer")
    st1 = parse_marker_int(t1, "refer_status")
    if d1 is None:
        checks.append(("X1 blind 전달", False, f"RTP delta 미출력(시나리오 미완) rc={rc1}"))
    else:
        a, b, c = d1
        ok1 = b >= FLOW_MIN and c >= FLOW_MIN and a <= DROP_MAX and (st1 is None or st1 // 100 == 2)
        checks.append(("X1 blind 전달", ok1,
                       f"refer_status={st1} recv A=+{a} B=+{b} C=+{c} (B·C≥{FLOW_MIN}, A≤{DROP_MAX}) rc={rc1}"))

    # ── X2: attended 전달 — B·C 미디어, A 드롭 ──
    rc2, _, d2 = run("transfer_attended")
    if d2 is None:
        checks.append(("X2 attended 전달", False, f"RTP delta 미출력(시나리오 미완) rc={rc2}"))
    else:
        a, b, c = d2
        ok2 = b >= FLOW_MIN and c >= FLOW_MIN and a <= DROP_MAX
        checks.append(("X2 attended 전달", ok2, f"recv A=+{a} B=+{b} C=+{c} (B·C≥{FLOW_MIN}, A≤{DROP_MAX}) rc={rc2}"))

    # ── X3: transfer_allowed=false → REFER 403, 원 통화 유지 ──
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    user_a = creds[0]["user"]
    if not _noxfer_seeded(ctx.dist_dir):
        checks.append(("X3 transfer_allowed=false 403", None,
                       f"접속서비스 '{NOXFER_SERVICE_REF}' 미시드 (S3-SEED with_noxfer) — 게이트 검사 생략"))
    else:
        orig_ref = get_service_ref(db_cfg, VOLTE_TABLE, user_a)
        if not orig_ref:
            checks.append(("X3 transfer_allowed=false 403", False, f"A={user_a} service_ref 조회 실패"))
        else:
            try:
                set_service_ref(db_cfg, VOLTE_TABLE, user_a, NOXFER_SERVICE_REF)
                notify_user_changed(ctx.sim_ip, user_a)
                rc3, t3, d3 = run("transfer")
                st3 = parse_marker_int(t3, "refer_status")
                if d3 is None:
                    checks.append(("X3 transfer_allowed=false 403", False, f"RTP delta 미출력(시나리오 미완) rc={rc3}"))
                else:
                    a, b, c = d3
                    ok3 = st3 == 403 and a >= FLOW_MIN and b >= FLOW_MIN and c <= DROP_MAX
                    checks.append(("X3 transfer_allowed=false 403", ok3,
                                   f"refer_status={st3} recv A=+{a} B=+{b} C=+{c} "
                                   f"(기대 403, 원 통화 A·B≥{FLOW_MIN} 유지, C≤{DROP_MAX}) rc={rc3}"))
            finally:
                set_service_ref(db_cfg, VOLTE_TABLE, user_a, orig_ref)
                notify_user_changed(ctx.sim_ip, user_a)

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))


def _via_tester(ctx: VerifyContext, done) -> ItemResult:
    """계측기 경로 — 전달 3검사를 동봉 시나리오로. X3 는 계획의 transferor 신원 전부를 transfer_allowed=false 서비스로 플립한 뒤 돈다."""
    cfg = tester_config()
    ctx.w(f"- 경로: 계측기 @ {cfg['url']} · 토폴로지 {cfg['topology']}")
    checks = [tester_check(ctx, "X1 blind 전달", "VOLTE-XFER-BLIND", f"{_RID}/X1", ht=4),
              tester_check(ctx, "X2 attended 전달", "VOLTE-XFER-ATTENDED", f"{_RID}/X2", ht=4)]
    if not _noxfer_seeded(ctx.dist_dir):
        checks.append(("X3 transfer_allowed=false 403", None,
                       f"접속서비스 '{NOXFER_SERVICE_REF}' 미시드 (S3-SEED with_noxfer) — 게이트 검사 생략"))
    else:
        db_cfg = _db.csp_db_config(ctx.dist_dir)
        try:
            users = (tester_role_identities(ctx, "VOLTE-XFER-DENIED", ht=4) or {}).get("transferor") or []
            if not users:
                raise RuntimeError("계획에 transferor 신원이 없다")
            orig = {u: get_service_ref(db_cfg, VOLTE_TABLE, u) for u in users}
            missing = [u for u, r in orig.items() if not r]
            if missing:
                raise RuntimeError(f"service_ref 조회 실패: {missing}")
        except Exception as e:
            checks.append(("X3 transfer_allowed=false 403", False, f"계측기 계획/신원 준비 실패: {e}"))
        else:
            try:
                for u in users:
                    set_service_ref(db_cfg, VOLTE_TABLE, u, NOXFER_SERVICE_REF)
                    notify_user_changed(ctx.sim_ip, u)
                checks.append(tester_check(ctx, "X3 transfer_allowed=false 403", "VOLTE-XFER-DENIED", f"{_RID}/X3", ht=4))
            finally:
                for u, r in orig.items():
                    set_service_ref(db_cfg, VOLTE_TABLE, u, r)
                    notify_user_changed(ctx.sim_ip, u)
    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, fmt_checks(checks))
