"""S6-SCN-PTT-VIDEO — PTT 그룹 영상 통화 (5인)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.tester import run_tester_scenario
from ._helpers import run_scenario, target_ip, local_ip_args


@verify_item(
    id="S6-SCN-PTT-VIDEO", stage=6, category="시나리오",
    name="PTT 그룹 영상 통화 (5인)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "stage6-ptt", "pipeline-full", "post-deploy"],
    side_effects=["sim-call"], timeout_s=90,
    execution_order=60,
)
def scn_ptt_video(ctx: VerifyContext) -> ItemResult:
    # 계측기가 설정돼 있으면 `PTT-GROUP-CALL-VIDEO`(토폴로지에 service: ptt 풀 필요). 없으면 cspsim 경로 (test_instrument.md §9)
    r = run_tester_scenario(ctx, "S6-SCN-PTT-VIDEO", "PTT 그룹 영상 통화 (5인)", "PTT-GROUP-CALL-VIDEO", stage=6, instances=1, ht=10)
    if r is not None:
        return r
    s = ctx.state
    _tgt = target_ip("psp", ctx.sim_ip)
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", _tgt,
        "-domain", s["PTT_DOM"], "-group", s["PTT_GROUP"],
    ]
    args += local_ip_args(_tgt)
    return run_scenario(ctx, "S6-SCN-PTT-VIDEO",
                        "PTT 그룹 영상 통화 (5인)", args,
                        ["PTT_USER", "PTT_GROUP"])
