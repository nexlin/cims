"""S3 dialog-event/당겨받기 BLF 회귀 (volte_supplementary_services.md §6.2).

관제 BLF 클릭 당겨받기의 표준 경로를 실측한다: C 가 B 를 dialog(RFC 4235) 구독 → A→B 링잉
시 서버가 dialog-info NOTIFY 로 링잉 leg Call-ID 를 전달 → C 가 그 Call-ID 로 INVITE-Replaces
(RFC 3891) → 서버가 원 relay 를 유지한 채 C 를 재고정해 A–C 연결. 두 표준(dialog-event 통지 +
Replaces 수신)이 한 흐름으로 엮인다.

검사:
  D1 dialog NOTIFY 수신 — C 가 dialog 구독 후 A→B 링잉 시 dialog-info NOTIFY 를 1건 이상 받는다
  D2 Replaces 재고정 — INVITE-Replaces 후 A·C 로 미디어가 흐르고(원 relay 승계) B(응답 보류)는 무흐름

같은 org(=픽업 그룹, dialog 구독 인가·Replaces 인가 모두 그룹 경계) VOIP 3명을 DB 에서 고른다.
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ._xfer_common import select_trio, trio_cred_args, parse_recv_delta, VOLTE_DOMAIN, FLOW_MIN, DROP_MAX

_RID = "S3-SCN-DIALOG"
_RNAME = "dialog-event/BLF 당겨받기 (RFC 4235 NOTIFY + RFC 3891 INVITE-Replaces)"

_NOTIFY_RE = re.compile(r"dialog_notify=(\d+)")


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=300,
    execution_order=65,
)
def dialog(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    creds, org = select_trio(ctx.dist_dir)
    if len(creds) < 3:
        ctx.w("- [SKIP] 같은 org(=픽업 그룹) VOIP 가입자 3명(A,B,C) 미확보")
        return done(ItemStatus.SKIP, "같은 org VOIP 3명 미확보")
    cred_a = trio_cred_args(creds, "dialog")
    media_dir = os.path.join(ctx.repo_root, "tests", "media")
    ctx.w(f"- 단말 org={org} A={creds[0]['user']} B={creds[1]['user']} C={creds[2]['user']}")

    args = [
        "-mode", "volte", "-scenario", "dialog_pickup", "-count", "3",
        "-ip", ctx.sim_ip, "-domain", VOLTE_DOMAIN,
        *cred_a, "-media_dir", media_dir, "-duration", "4", "-no_video",
    ]
    rc, tail = run_cspsim(ctx.repo_root, args, timeout=180)

    m = _NOTIFY_RE.search(tail)
    n_notify = int(m.group(1)) if m else 0
    d = parse_recv_delta(tail)

    checks = []
    checks.append(("D1 dialog NOTIFY 수신", n_notify >= 1, f"dialog_notify={n_notify} rc={rc}"))
    if d is None:
        checks.append(("D2 Replaces 재고정", False, f"RTP delta 미출력(시나리오 미완) rc={rc}"))
    else:
        a, b, c = d
        ok = a >= FLOW_MIN and c >= FLOW_MIN and b <= DROP_MAX
        checks.append(("D2 Replaces 재고정", ok, f"recv A=+{a} B=+{b} C=+{c} (A·C≥{FLOW_MIN}, B≤{DROP_MAX})"))

    all_ok = all(ok for _, ok, _ in checks)
    for name, ok, detail in checks:
        ctx.w(f"- [{'PASS' if ok else 'FAIL'}] {name} — {detail}")
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL,
                "\n".join(f"{'PASS' if ok else 'FAIL'} {name}: {dt}" for name, ok, dt in checks))
