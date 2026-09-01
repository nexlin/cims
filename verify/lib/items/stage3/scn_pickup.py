"""S3 당겨받기(call pickup) 회귀 — 링잉 호를 픽업 단말로 재고정 (volte_supplementary_services.md §5).

A→B 가 링잉(B 는 응답 보류) 중 픽업 단말이 당겨받기 코드를 다이얼하면, 서버 PickUp 이 B 의
링잉 leg 를 회수하고 그 자리를 픽업 단말로 재키잉해 A 와 연결한다(RELAY_MODIFY peer1).
그룹 축은 `pickup_group`(§5.1) — 컬럼이 있으면 A,B,C 에 같은 그룹·D 에 다른 그룹(같은 org)을
명시 부여해 실컬럼 축으로 검증하고, 컬럼이 없으면 org 폴백 축(P3/P4 SKIP).

검사 (판정 정본 = 재고정 후 누적 수신 RTP delta + 픽업 INVITE 최종 응답 `pickup_status`):
  P1 그룹 픽업   — C 가 `**` 다이얼 → A·C 미디어, B(응답 보류) 무흐름
  P2 지정 픽업   — C 가 `**<B>` 다이얼 → A·C 미디어, B 무흐름 (피처코드 prefix 파싱 §5.2)
  P3 그룹 경계   — D(다른 pickup_group, 같은 org)가 `**<B>` 다이얼 → 403, 미디어 재고정 없음
  P4 그룹 밖 픽업 — D 가 `**` 다이얼 → 404 (D 의 그룹에 링잉 호 없음)

피처코드는 접속서비스 `pickup_feature_code`(S3-SEED 가 volte 서비스에 "**" 시드)에서 읽는다.
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

_RID = "S3-SCN-PICKUP"
_RNAME = "당겨받기 (링잉 호 재고정 — pickup_group 축·피처코드·RELAY_MODIFY)"


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call", "db-write", "service-signal"], timeout_s=480,
    execution_order=64,
)
def pickup(ctx: VerifyContext) -> ItemResult:
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

    def run(trio: list, tag: str, target: str = "") -> tuple:
        args = [
            "-mode", "volte", "-scenario", "pickup", "-count", "3",
            "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
            *trio_cred_args(trio, tag), "-media_dir", media_dir, "-duration", "4", "-no_video",
            "-pickup_code", "**",
        ]
        if target:
            args += ["-pickup_target", target]
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)
        return rc, parse_recv_delta(tail), parse_marker_int(tail, "pickup_status")

    def media_ok(d) -> bool:  # A(발신)·C(픽업 단말) 로 미디어, B(응답 보류) 는 무흐름
        return d is not None and d[0] >= FLOW_MIN and d[2] >= FLOW_MIN and d[1] <= DROP_MAX

    def media_str(d) -> str:
        return "RTP delta 미출력" if d is None else f"recv A=+{d[0]} B=+{d[1]} C=+{d[2]}"

    checks = []
    grp = f"vfy-pg-{org}"
    with PickupGroupFixture(ctx.dist_dir, ctx.sim_ip,
                            {A["user"]: grp, B["user"]: grp, C["user"]: grp, D["user"]: grp + "-x"}) as fx:
        ctx.w(f"- 그룹 축: {fx.axis}")

        # ── P1: 그룹 픽업 ──
        rc, d, st = run([A, B, C], "pickup_p1")
        checks.append(("P1 그룹 픽업", media_ok(d) and st == 200,
                       f"pickup_status={st} {media_str(d)} (A·C≥{FLOW_MIN}, B≤{DROP_MAX}) rc={rc}"))

        # ── P2: 지정 픽업 (같은 그룹) ──
        rc, d, st = run([A, B, C], "pickup_p2", target=B["user"])
        checks.append(("P2 지정 픽업 **<B>", media_ok(d) and st == 200,
                       f"pickup_status={st} {media_str(d)} (A·C≥{FLOW_MIN}, B≤{DROP_MAX}) rc={rc}"))

        if not fx.active:
            checks.append(("P3 타 그룹 지정 픽업 403", None, "pickup_group 컬럼 부재 — org 폴백 축에서는 그룹 경계 검사 불가"))
            checks.append(("P4 그룹 밖 그룹 픽업 404", None, "pickup_group 컬럼 부재 — org 폴백 축에서는 그룹 경계 검사 불가"))
        else:
            # ── P3: D(다른 그룹) 지정 픽업 → 403, 재고정 없음 ──
            rc, d, st = run([A, B, D], "pickup_p3", target=B["user"])
            no_media = d is not None and d[2] <= DROP_MAX
            checks.append(("P3 타 그룹 지정 픽업 403", st == 403 and no_media,
                           f"pickup_status={st} {media_str(d)} (기대 403, D≤{DROP_MAX}) rc={rc}"))
            # ── P4: D 그룹 픽업 → 404 (D 그룹에 링잉 호 없음) ──
            rc, d, st = run([A, B, D], "pickup_p4")
            no_media = d is not None and d[2] <= DROP_MAX
            checks.append(("P4 그룹 밖 그룹 픽업 404", st == 404 and no_media,
                           f"pickup_status={st} {media_str(d)} (기대 404, D≤{DROP_MAX}) rc={rc}"))

    all_ok = emit_checks(ctx, checks)
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL, f"axis={fx.axis}\n" + fmt_checks(checks))
