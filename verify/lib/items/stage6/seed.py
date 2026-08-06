"""S6-SEED — 가입자/그룹 선택 + access_services.jsonl 시드 + 배포본 csp reload.

Stage 6 는 배포본 csp (build/dist/volte-sip-server/csp/, ptt-sip-server/psp/) 대상.
config 경로/PID 파일 위치가 Stage 3 (build/dist/csp/) 와 다르다.
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
from ...common.cmp_client import cmp_stats
from ...common.ibcf_routing import seed_ibcf_routing, IBCF_PEER_DOMAIN

# Mock 외부 peer (cspsim peer 프로세스) 의 listen endpoint.
# loopback alias 부담 회피 위해 127.0.0.1 의 unused port 사용.
IBCF_MOCK_PEER_IP   = "127.0.0.1"
IBCF_MOCK_PEER_PORT = 6800
from ..stage5._native_steps import _INSTANCES as _NATIVE_INSTANCES


def _wait_cmp_ready(ip: str = "127.0.0.1", port: int = 9000,
                    timeout_sec: int = 60) -> bool:
    """cmp 가 STATS 에 응답할 때까지 polling. 응답 시 True."""
    import time as _t
    deadline = _t.time() + timeout_sec
    while _t.time() < deadline:
        if cmp_stats(ip=ip, port=port, timeout=1.0) is not None:
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
        details = (cmp_stats(ip=ip, port=port, timeout=1.0) or {}).get("group_details", []) or []
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
    execution_order=20,
)
def seed(ctx: VerifyContext) -> ItemResult:
    # count 는 stage6 시나리오의 cspsim -count 와 일치 (volte 2 / ptt 5) —
    # cspsim 이 비밀번호 하나로 그 수만큼 단말을 만든다.
    sub = select_subscribers(_db.csp_db_config(ctx.dist_dir), voip_count=2, ptt_count=5)

    # _INSTANCES 의 모든 csp variant (CSP/PSP/ISP) 에 access_services.jsonl 시드
    # + SIGUSR1 reload. 누락 시 해당 인스턴스의 gclsServiceMap 이 빈 채로 유지되어
    # cspsim REGISTER 가 'data 불완전 — Auth reject' 으로 403 거부.
    # filter: tarball 이 csp/psp/isp — 모두 csp 바이너리 사용. dir 필드는
    # install_path leaf (psp 등) 라 매칭에 부적합.
    csp_variants = [inst for inst in _NATIVE_INSTANCES
                     if inst.get("tarball") in ("csp", "psp", "isp")]
    seed_lines: list = []
    primary_seeded = 0
    primary_reloaded = False
    for inst in csp_variants:
        # install_path = dist_dir/<agent_name> (server level — 변종 tarball 안 dir
        # 디렉토리가 그 안에 풀려 install_path/<dir>/config/<dir>.json 가 됨).
        # cims.sh DIST_DIR = install_path. PID 파일은 PID_DIR=$DIST_DIR/run.
        # access_services.jsonl 시드 위치는 csp ELF 의 ConfigJsonlDir fallback
        # 정합: csp.json 의 부모×3 + "/config" → install_path/config (server
        # level 의 config/). 한 server 에 한 변종만 install 되는 P1 토폴로지에서
        # 안전. (csp.json 에 ConfigJsonlDir 명시 안 됨 → fallback 동작.)
        install_path = os.path.join(ctx.dist_dir, inst["agent_name"])
        cfg_dir = os.path.join(install_path, "config")
        pid_file = os.path.join(install_path, "run", f"{inst['id']}.pid")
        # ISP 는 IBCF role only — access_services (voip/ptt) 시드는 IBCF 흐름과
        # 무관 (ISP 의 CSCF=false 라 REGISTER 자체를 받지 않음).
        # P2 토폴로지에서 isp 와 csp 가 같은 install_path/config/ 를 공유하므로
        # ISP 측에서 access_services 를 건드리면 안 됨 (CSP 의 시드를 보존).
        # routing 6종만 시드 (rule이 req_uri_host contains "trunk.peer.test" 라
        # CSP 의 VoLTE 호 흐름은 매칭 안 되므로 sharing 무해).
        if inst["id"] == "isp":
            n = 0   # ISP 는 access_services 시드 skip (공유 dir 보존)
        else:
            n = seed_access_services(
                cfg_dir, sub["voip_ref"], sub["ptt_ref"],
                tag="verify-stage6-seed",
                note=f"auto-seeded by cims_verify S6-SEED ({inst['id']})",
            )
        # ISP: IBCF routing 6종 추가 시드. CSP/PSP 는 IBCF role off 라 무의미.
        ibcf_n = 0
        if inst["id"] == "isp":
            ibcf_n = seed_ibcf_routing(
                cfg_dir,
                isp_local_ip=inst["local_ip"],
                isp_local_port=inst["listen"][0],
                peer_ip=IBCF_MOCK_PEER_IP,
                peer_port=IBCF_MOCK_PEER_PORT,
            )
        reloaded = signal_csp_reload(pid_file)
        ibcf_note = f" / ibcf {ibcf_n}콜렉션" if ibcf_n else ""
        seed_lines.append(
            f"  · {inst['id']}: seeded {n}건{ibcf_note} / reload(SIGUSR1)={'OK' if reloaded else 'FAIL'}"
        )
        if inst["id"] == "csp":
            primary_seeded = n
            primary_reloaded = reloaded
    seeded_n = primary_seeded
    reloaded = primary_reloaded

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
        f"- 시드 대상: {len(csp_variants)} csp variant",
        *seed_lines,
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
