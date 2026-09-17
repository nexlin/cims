"""S6-SCN-VOLTE-VOICE — VoLTE 음성 2자 통화."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.subscribers import cred_args
from ...common.tester import run_tester_scenario
from ._helpers import run_scenario, target_ip, local_ip_args


@verify_item(
    id="S6-SCN-VOLTE-VOICE", stage=6, category="시나리오",
    name="VoLTE 음성 2자 통화",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "stage6-volte", "pipeline-full", "post-deploy"],
    side_effects=["sim-call"], timeout_s=60,
    execution_order=30,
)
def scn_volte_voice(ctx: VerifyContext) -> ItemResult:
    # 계측기가 설정돼 있으면(CIMS_TESTER_URL·CIMS_TESTER_TOPOLOGY) 그쪽으로 — 판정 = 계측기 verdict(RFC 6076 기대치 + 대상 증거).
    #   없으면 기존 cspsim 경로(녹취 delta 판정). test_instrument.md §9 단계적 이전.
    r = run_tester_scenario(ctx, "S6-SCN-VOLTE-VOICE", "VoLTE 음성 2자 통화", "VOLTE-CALL-BASIC", stage=6, instances=1, ht=5)
    if r is not None:
        return r
    s = ctx.state
    _tgt = target_ip("csp", ctx.sim_ip)
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", _tgt,
        "-user", s["VOIP_USER"], "-domain", s["VOIP_DOM"],
        *cred_args(s, "VOIP", 2), "-no_video",
    ]
    args += local_ip_args(_tgt)
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    return run_scenario(ctx, "S6-SCN-VOLTE-VOICE",
                        "VoLTE 음성 2자 통화", args, ["VOIP_USER"])
