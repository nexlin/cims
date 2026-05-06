"""S3-SCN-PTT-SMOKE — PTT 그룹 통화 스모크 (5인, dev CSP)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.subscribers import MCPTT_DOMAIN
from ...common.cmp_client import remove_group
from ._helpers import run_scenario


@verify_item(
    id="S3-SCN-PTT-SMOKE",
    stage=3, category="시나리오",
    name="PTT 그룹 통화 (5인, dev CSP)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=180,
)
def ptt_smoke(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    # 모듈 검증의 그룹 세션 잔여로 INVITE 가 410 Gone 받는 것 방지.
    remove_group(s.get("PTT_GROUP", ""))
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-user", s.get("PTT_USER", ""),
        "-domain", s.get("PTT_DOM", MCPTT_DOMAIN),
        "-password", s.get("PTT_PWD", ""),
        "-group", s.get("PTT_GROUP", ""),
    ]
    return run_scenario(ctx, "S3-SCN-PTT-SMOKE",
                        "PTT 그룹 통화 (5인)", args, ["PTT_USER", "PTT_GROUP"])
