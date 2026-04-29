"""P1-REGRESS-VOIP — VoIP 2자 통화 회귀 (B2BUA, Test-CSP)."""
from __future__ import annotations

from ....registry import verify_item, ItemResult
from ....context import VerifyContext
from ....common.subscribers import VOLTE_DOMAIN
from ._helpers import run_scenario


@verify_item(
    id="P1-REGRESS-VOIP",
    phase=1, category="시나리오",
    name="VoIP 2자 통화 (B2BUA, Test-CSP)",
    depends_on=["P1-SEED"],
    presets=["phase1-full"],
    side_effects=["sim-call"], timeout_s=120,
)
def regress_voip(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", ctx.sim_ip,
        "-user", s.get("VOIP_USER", ""),
        "-domain", s.get("VOIP_DOM", VOLTE_DOMAIN),
        "-password", s.get("VOIP_PWD", ""),
    ]
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    return run_scenario(ctx, "P1-REGRESS-VOIP",
                        "VoIP 2자 통화 (B2BUA)", args, ["VOIP_USER"])
