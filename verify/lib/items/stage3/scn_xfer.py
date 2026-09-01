"""S3 호 전달(REFER) 회귀 — blind/attended 전달의 미디어 재고정 (volte_supplementary_services.md §6).

A→B 통화를 세운 뒤 A 가 REFER 로 상대를 C 에게 넘긴다. 서버(B2BUA)가 REFER 를 종단하고
원 통화의 relay 세션을 유지한 채 교체되는 leg 만 RELAY_MODIFY 로 재고정한다(포트 산술 금지).
검증 정본은 전달 후 각 단말의 누적 수신 RTP delta:
  X1 blind 전달   — A blind REFER(→C) 후 B·C 로 미디어가 흐르고(원 relay 승계) A(전달자)는 드롭
  X2 attended 전달 — A→B + A→C(상담) 후 attended REFER(Replaces): B·C 로 미디어, A 드롭

cspsim 3 단말(A,B,C)은 같은 org(픽업 그룹 축 무관)이며 dev DB 가입자에서 고른다.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ._xfer_common import select_trio, trio_cred_args, parse_recv_delta, VOLTE_DOMAIN, FLOW_MIN, DROP_MAX

import os

_RID = "S3-SCN-XFER"
_RNAME = "호 전달 (blind/attended REFER — 미디어 재고정·원 relay 승계)"


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=400,
    execution_order=63,
)
def xfer(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

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

    checks = []  # (이름, ok, 상세)

    # ── X1: blind 전달 — B·C 미디어, A 드롭 ──
    rc1, _, d1 = run("transfer")
    if d1 is None:
        checks.append(("X1 blind 전달", False, f"RTP delta 미출력(시나리오 미완) rc={rc1}"))
    else:
        a, b, c = d1
        ok1 = b >= FLOW_MIN and c >= FLOW_MIN and a <= DROP_MAX
        checks.append(("X1 blind 전달", ok1, f"recv A=+{a} B=+{b} C=+{c} (B·C≥{FLOW_MIN}, A≤{DROP_MAX}) rc={rc1}"))

    # ── X2: attended 전달 — B·C 미디어, A 드롭 ──
    rc2, _, d2 = run("transfer_attended")
    if d2 is None:
        checks.append(("X2 attended 전달", False, f"RTP delta 미출력(시나리오 미완) rc={rc2}"))
    else:
        a, b, c = d2
        ok2 = b >= FLOW_MIN and c >= FLOW_MIN and a <= DROP_MAX
        checks.append(("X2 attended 전달", ok2, f"recv A=+{a} B=+{b} C=+{c} (B·C≥{FLOW_MIN}, A≤{DROP_MAX}) rc={rc2}"))

    all_ok = all(ok for _, ok, _ in checks)
    for name, ok, detail in checks:
        ctx.w(f"- [{'PASS' if ok else 'FAIL'}] {name} — {detail}")
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL,
                "\n".join(f"{'PASS' if ok else 'FAIL'} {name}: {d}" for name, ok, d in checks))
