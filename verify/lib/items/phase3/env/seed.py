"""P3-SEED — 가입자/그룹 선택 + access_services.jsonl 시드 + csp reload.

Phase 3 는 배포본 csp (build/dist/csp-server/csp/) 대상. config 경로/PID 파일 위치가
Phase 1 (build/dist/csp/) 과 다르다.
"""
from __future__ import annotations

import os

from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext
from ....common import db as _db
from ....common.subscribers import (
    select_subscribers, VOLTE_DOMAIN, MCPTT_DOMAIN,
)
from ....common.access_services import seed_access_services, signal_csp_reload


@verify_item(
    id="P3-SEED",
    phase=3, category="환경",
    name="가입자/그룹 선택 + access_services.jsonl 시드 + csp reload",
    depends_on=["P3-ENTRY-CHECK"],
    presets=["phase3-full"],
    side_effects=["fs-write", "service-signal"],
    timeout_s=30,
)
def seed(ctx: VerifyContext) -> ItemResult:
    cfg_dir  = os.path.join(ctx.dist_dir, "csp-server", "csp", "config")
    pid_file = os.path.join(ctx.dist_dir, "csp-server", "csp", "run", "csp.pid")

    sub = select_subscribers(_db.csp_db_config(ctx.dist_dir))
    seeded_n = seed_access_services(
        cfg_dir, sub["voip_ref"], sub["ptt_ref"],
        tag="verify-phase3-seed",
        note="auto-seeded by cims_verify P3-SEED",
    )
    reloaded = signal_csp_reload(pid_file)

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
    ]
    ctx.w("## P3-SEED — 시나리오 준비 (가입자/시드/reload)")
    for line in lines:
        ctx.w(line)
    ctx.w()
    return ItemResult(
        id="P3-SEED", name="가입자/그룹 선택 + access_services.jsonl 시드",
        status=ItemStatus.PASS, detail="\n".join(lines), phase=3,
    )
