"""S3-SCN-PTT-SMOKE — PTT 그룹 통화 스모크 (5인, dev CSP)."""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ...common.subscribers import MCPTT_DOMAIN, cred_args
from ._helpers import run_scenario


@verify_item(
    id="S3-SCN-PTT-SMOKE",
    stage=3, category="시나리오",
    name="PTT 그룹 통화 (5인, dev CSP)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sim-call"], timeout_s=180,
    execution_order=60,
)
def ptt_smoke(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    # NOTE: 과거에는 여기서 cmp_client.remove_group 을 호출해 CMP 의 그룹
    # 세션을 사전 정리했지만, CSP 의 m_mapPttSession 캐시는 그대로라
    # 다음 INVITE 처리 시 CSP 가 ADD_PTT_GROUP 을 보내지 않고 곧장
    # JOIN_PTT_GROUP 으로 가서 'Group Not Found' 가 났음. CSP 가 startup 에
    # 모든 그룹을 ADD_PTT_GROUP 으로 push 하므로 fresh CSP/CMP 에서는
    # 사전 정리가 불필요. 재실행 케이스의 stale 그룹 세션은 prep-reset
    # (cims.sh reset --all) 에서 CSP/CMP 동시 재시작으로 정리.
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-user", s.get("PTT_USER", ""),
        "-domain", s.get("PTT_DOM", MCPTT_DOMAIN),
        # 단말별 자격 파일(-no-db -creds) 우선 — 시드 창을 그대로 전개한다.
        # 창 미확보 시 -password 폴백(DB 모드 — -user 는 무시되고 DB 첫 N 행 사용).
        *cred_args(s, "PTT", 5),
        "-group", s.get("PTT_GROUP", ""),
    ]
    return run_scenario(ctx, "S3-SCN-PTT-SMOKE",
                        "PTT 그룹 통화 (5인)", args, ["PTT_USER", "PTT_GROUP"])
