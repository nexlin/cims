"""S6-SCN-VOLTE-VOICE — VoLTE 음성 2자 통화."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
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
    s = ctx.state
    _tgt = target_ip("csp", ctx.sim_ip)
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", _tgt,
        "-user", s["VOIP_USER"], "-domain", s["VOIP_DOM"],
        "-password", s["VOIP_PWD"], "-no_video",
    ]
    args += local_ip_args(_tgt)
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    return run_scenario(ctx, "S6-SCN-VOLTE-VOICE",
                        "VoLTE 음성 2자 통화", args, ["VOIP_USER"])
