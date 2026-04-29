"""P3-SCN-PTT-VOICE — PTT 그룹 음성 통화 (5인)."""
from __future__ import annotations

from ....registry import verify_item, ItemResult
from ....context import VerifyContext
from ._helpers import run_scenario


@verify_item(
    id="P3-SCN-PTT-VOICE", phase=3, category="시나리오",
    name="PTT 그룹 음성 통화 (5인)",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=90,
)
def scn_ptt_voice(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-domain", s["PTT_DOM"], "-group", s["PTT_GROUP"], "-no_video",
    ]
    return run_scenario(ctx, "P3-SCN-PTT-VOICE",
                        "PTT 그룹 음성 통화 (5인)", args,
                        ["PTT_USER", "PTT_GROUP"])
