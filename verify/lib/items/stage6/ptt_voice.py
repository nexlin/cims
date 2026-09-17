"""S6-SCN-PTT-VOICE — PTT 그룹 음성 통화 (5인)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.tester import run_tester_scenario
from ._helpers import run_scenario, target_ip, local_ip_args


@verify_item(
    id="S6-SCN-PTT-VOICE", stage=6, category="시나리오",
    name="PTT 그룹 음성 통화 (5인)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "stage6-ptt", "pipeline-full", "post-deploy"],
    side_effects=["sim-call"], timeout_s=90,
    execution_order=50,
)
def scn_ptt_voice(ctx: VerifyContext) -> ItemResult:
    # 계측기가 설정돼 있으면(CIMS_TESTER_URL·CIMS_TESTER_TOPOLOGY) 그쪽으로 — 그룹 세션 시나리오(등록·affiliation·그룹 INVITE fan-out·
    #   floor 요청/해제·RTP 표본·BYE)의 verdict 가 판정. 토폴로지에 service=ptt 풀(그룹 멤버 신원)이 있어야 한다. 없으면 기존 cspsim 경로.
    r = run_tester_scenario(ctx, "S6-SCN-PTT-VOICE", "PTT 그룹 음성 통화", "PTT-GROUP-CALL-BASIC", stage=6, instances=1, ht=10,
                            state_prefix="S6_PTT_VOICE")
    if r is not None:
        return r
    s = ctx.state
    _tgt = target_ip("psp", ctx.sim_ip)
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", _tgt,
        "-domain", s["PTT_DOM"], "-group", s["PTT_GROUP"], "-no_video",
    ]
    args += local_ip_args(_tgt)
    return run_scenario(ctx, "S6-SCN-PTT-VOICE",
                        "PTT 그룹 음성 통화 (5인)", args,
                        ["PTT_USER", "PTT_GROUP"],
                        state_prefix="S6_PTT_VOICE")
