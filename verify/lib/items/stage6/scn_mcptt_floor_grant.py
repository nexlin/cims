"""S6-MCPTT-FLOOR-GRANT — PTT 그룹콜 floor signaling 검증.

S6-SCN-PTT-VOICE 가 5인 그룹 rotation 시나리오를 돌리며 CMP `PMcpttGroup` 가
floor REQUEST/GRANT/RELEASE/IDLE 패킷을 처리한다 (RTCP APP, op-code
REQUEST=1 / GRANT=2 / RELEASE=4 / IDLE=5). 본 항목은 그 직후 read-only 로
다음 두 시그널을 본다.

1차) CMP `*.flow.jsonl` 의 proto="MCPTT" method="FLOOR_GRANT/FLOOR_TAKEN/
     FLOOR_IDLE" 라인 카운트.
2차 fallback) cspsim stdout tail 에 "PTT Request (floor)" / "Floor rotation
     complete" 마커.

LogFlow.Floor 가 비활성이거나 환경 미구성이면 SKIP.
"""
from __future__ import annotations

import collections
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.sip_log import iter_flow_lines


_FLOOR_METHODS = ("FLOOR_GRANT", "FLOOR_TAKEN", "FLOOR_IDLE",
                  "FLOOR_REQUEST", "FLOOR_REJECT", "FLOOR_RELEASE")


@verify_item(
    id="S6-MCPTT-FLOOR-GRANT", stage=6, category="시나리오",
    name="MCPTT floor REQUEST/GRANT/RELEASE/IDLE 시그널 검증",
    depends_on=["S6-SCN-PTT-VOICE"],
    presets=["stage6-full", "stage6-ptt", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=15,
    execution_order=51,
)
def scn_mcptt_floor_grant(ctx: VerifyContext) -> ItemResult:
    since = ctx.state.get("S6_PTT_VOICE_T0") or (time.time() - 180)
    flow_msgs = list(
        iter_flow_lines(ctx.dist_dir, node="cmp", proto="MCPTT", since=since)
    )
    methods = collections.Counter(
        m.get("method", "") for m in flow_msgs if m.get("method") in _FLOOR_METHODS
    )
    grant = methods.get("FLOOR_GRANT", 0)
    taken = methods.get("FLOOR_TAKEN", 0)
    idle = methods.get("FLOOR_IDLE", 0)
    ok_flow = (grant >= 1) and (taken >= 1) and (idle >= 1)

    tail = ctx.state.get("S6_PTT_VOICE_TAIL", "") or ""
    sim_req = "PTT Request (floor)" in tail
    sim_done = "Floor rotation complete" in tail
    ok_sim = sim_req and sim_done

    notes = [
        f"- CMP flow MCPTT 라인: total={len(flow_msgs)} "
        f"GRANT={grant} TAKEN={taken} IDLE={idle}",
        f"- cspsim 마커: request={sim_req} rotation_done={sim_done}",
    ]

    if grant == 0 and not sim_req:
        notes.append("- [SKIP] flow.jsonl 비활성 + cspsim 마커 없음 — "
                     "LogFlow.Floor=false 또는 PTT-VOICE 미실행")
        ctx.w("### S6-MCPTT-FLOOR-GRANT — SKIP")
        for n in notes:
            ctx.w(n)
        ctx.w()
        return ItemResult(
            id="S6-MCPTT-FLOOR-GRANT", name="MCPTT floor 시그널 검증",
            status=ItemStatus.SKIP, stage=6, detail="\n".join(notes),
        )

    ok = ok_flow or ok_sim
    ctx.w("### S6-MCPTT-FLOOR-GRANT — MCPTT floor 시그널 검증")
    for n in notes:
        ctx.w(n)
    ctx.w()
    return ItemResult(
        id="S6-MCPTT-FLOOR-GRANT", name="MCPTT floor 시그널 검증",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        stage=6, detail="\n".join(notes),
    )
