"""P3-SCN-VOLTE-VIDEO — VoLTE 영상 2자 통화."""
from __future__ import annotations

from ....registry import verify_item, ItemResult
from ....context import VerifyContext
from ._helpers import run_scenario


@verify_item(
    id="P3-SCN-VOLTE-VIDEO", phase=3, category="시나리오",
    name="VoLTE 영상 2자 통화",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=60,
)
def scn_volte_video(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", ctx.sim_ip,
        "-user", s["VOIP_USER"], "-domain", s["VOIP_DOM"],
        "-password", s["VOIP_PWD"],
    ]
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    return run_scenario(ctx, "P3-SCN-VOLTE-VIDEO",
                        "VoLTE 영상 2자 통화", args, ["VOIP_USER"])
