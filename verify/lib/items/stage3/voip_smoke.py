"""S3-SCN-VOIP-SMOKE — VoIP 2자 통화 회귀 (B2BUA, dev CSP)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.subscribers import VOLTE_DOMAIN, cred_args
from ._helpers import run_scenario


@verify_item(
    id="S3-SCN-VOIP-SMOKE",
    stage=3, category="시나리오",
    name="VoIP 2자 통화 (B2BUA, dev CSP)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=120,
    execution_order=50,
)
def voip_smoke(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", ctx.sim_ip,
        "-user", s.get("VOIP_USER", ""),
        "-domain", s.get("VOIP_DOM", VOLTE_DOMAIN),
        # 단말별 자격 파일(-creds) 우선 — 자격 파일의 authId 가 -auth_id 전개를 대체한다.
        *cred_args(s, "VOIP", 2),
    ]
    if s.get("VOIP_AUTH"):
        args += ["-auth_id", s["VOIP_AUTH"]]
    return run_scenario(ctx, "S3-SCN-VOIP-SMOKE",
                        "VoIP 2자 통화 (B2BUA)", args, ["VOIP_USER"])
