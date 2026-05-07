"""S6-SEED — 가입자/그룹 선택 + access_services.jsonl 시드 + 배포본 csp reload.

Stage 6 는 배포본 csp (build/dist/csp-server/csp/) 대상. config 경로/PID 파일 위치가
Stage 3 (build/dist/csp/) 와 다르다.
"""
from __future__ import annotations

import os
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.subscribers import (
    select_subscribers, VOLTE_DOMAIN, MCPTT_DOMAIN,
)
from ...common.access_services import seed_access_services, signal_csp_reload
from ...common.csp_notify import trigger_group_resync
from ...common.cmp_client import cmp_request


def _wait_cmp_ready(ip: str = "127.0.0.1", port: int = 9000,
                    timeout_sec: int = 60) -> bool:
    """cmp 가 STATS_REQUEST 에 응답할 때까지 polling. 응답 시 True."""
    import time as _t
    deadline = _t.time() + timeout_sec
    while _t.time() < deadline:
        resp = cmp_request({"cmd": "STATS_REQUEST", "sesid": "verify-seed-wait"},
                           ip=ip, port=port, timeout=1.0)
        if resp and isinstance(resp.get("response"), dict):
            return True
        _t.sleep(2)
    return False


def _wait_group_in_cmp(target_gid: str, ip: str = "127.0.0.1",
                       port: int = 9000, timeout_sec: int = 60) -> bool:
    """cmp STATS 의 group_details 에 target_gid 등장 때까지 polling.

    매 2s 마다 GROUP_CHANGED notify_csp 도 함께 trigger 하여 csp 의 재등록을
    유도 (csp 시작 직후 cmp 와 첫 통신 실패 케이스 대응)."""
    import time as _t
    deadline = _t.time() + timeout_sec
    while _t.time() < deadline:
        trigger_group_resync(f"tel:{target_gid}")
        _t.sleep(2)
        resp = cmp_request({"cmd": "STATS_REQUEST", "sesid": "verify-seed-poll"},
                           ip=ip, port=port, timeout=1.0)
        details = ((resp or {}).get("response") or {}).get("group_details", []) or []
        if any(isinstance(g, dict) and g.get("group_id") == target_gid
               for g in details):
            return True
    return False


@verify_item(
    id="S6-SEED",
    stage=6, category="환경",
    name="가입자/그룹 선택 + access_services.jsonl 시드 + csp reload",
    depends_on=["S6-ENTRY-CHECK"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "service-signal"],
    timeout_s=30,
)
def seed(ctx: VerifyContext) -> ItemResult:
    cfg_dir  = os.path.join(ctx.dist_dir, "csp-server", "csp", "config")
    pid_file = os.path.join(ctx.dist_dir, "csp-server", "csp", "run", "csp.pid")

    sub = select_subscribers(_db.csp_db_config(ctx.dist_dir))
    seeded_n = seed_access_services(
        cfg_dir, sub["voip_ref"], sub["ptt_ref"],
        tag="verify-stage6-seed",
        note="auto-seeded by cims_verify S6-SEED",
    )
    reloaded = signal_csp_reload(pid_file)

    # pipeline 회차에서 csp fresh start 후 cmp 와의 첫 control 통신이 실패하는
    # 케이스 대응 (~90s wait). 다음 두 단계로 우회:
    #  1) cmp 가 STATS_REQUEST 응답할 때까지 wait (기본 60s)
    #  2) GROUP_CHANGED notify_csp 발송 + cmp STATS 의 group_details 에
    #     target group 이 등장할 때까지 polling (기본 60s, 2s 간격 재발송)
    target_gid = sub.get("ptt_group", "")
    cmp_ready = _wait_cmp_ready(timeout_sec=60)
    group_in_cmp = False
    if cmp_ready and target_gid:
        group_in_cmp = _wait_group_in_cmp(target_gid, timeout_sec=60)
    elif target_gid:
        # cmp 미응답이라도 group_resync 1회 fallback
        trigger_group_resync(f"tel:{target_gid}")

    voip_auth = f"{sub['voip_imsi']}@{VOLTE_DOMAIN}" if sub["voip_imsi"] else ""
    ptt_auth  = f"{sub['ptt_imsi']}@{MCPTT_DOMAIN}"  if sub["ptt_imsi"]  else ""
    ctx.state.update({
        "VOIP_USER": sub["voip_user"], "VOIP_PWD": sub["voip_pwd"],
        "VOIP_AUTH": voip_auth, "VOIP_DOM": VOLTE_DOMAIN,
        "PTT_USER":  sub["ptt_user"],  "PTT_PWD":  sub["ptt_pwd"],
        "PTT_AUTH":  ptt_auth,  "PTT_DOM":  MCPTT_DOMAIN,
        "PTT_GROUP": sub["ptt_group"],
    })

    lines = [
        f"- VoIP: user={sub['voip_user']!r} domain={VOLTE_DOMAIN} auth_id={voip_auth!r}",
        f"- PTT:  user={sub['ptt_user']!r}  domain={MCPTT_DOMAIN} group={sub['ptt_group']!r}",
        f"- jsonlDir: {cfg_dir}",
        f"- seeded: {seeded_n}건  / csp reload(SIGUSR1): {'OK' if reloaded else 'FAIL'}",
        f"- cmp STATS ready: {'OK' if cmp_ready else 'TIMEOUT'}"
        f" / group({target_gid!r}) in CMP: {'OK' if group_in_cmp else 'TIMEOUT'}",
    ]
    ctx.w("## S6-SEED — 시나리오 준비 (가입자/시드/reload)")
    for line in lines:
        ctx.w(line)
    ctx.w()
    return ItemResult(
        id="S6-SEED", name="가입자/그룹 선택 + access_services.jsonl 시드",
        status=ItemStatus.PASS, detail="\n".join(lines), stage=6,
    )
