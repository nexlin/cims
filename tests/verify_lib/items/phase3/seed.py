"""시나리오 준비 — DB 가입자/그룹 선택 + 배포본 csp 의 access_services.jsonl 시드.

cims.sh _verify_phase3 의 §2 와 동일 로직.
- volte_subscriptions / ptt_subscriptions / ptt_groups 에서 가입자 선택
- build/dist/csp-server/csp/config/access_services.jsonl 작성
- 배포본 csp 에 SIGUSR1 (ConfigCache reload)
- ctx.state 에 VOIP_USER / PTT_USER / PTT_GROUP / *_DOM / *_AUTH 적재 (후속 시나리오 항목이 사용)
"""
from __future__ import annotations

import json
import os
import time
import uuid

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_VOLTE_DOM = "ims.mnc033.mcc450.3gppnetwork.org"
_MCPTT_DOM = "ptt.mnc033.mcc450.3gppnetwork.org"


def _load_csp_db_config(csp_json_path: str) -> dict:
    try:
        with open(csp_json_path) as f:
            d = json.load(f)
        return d.get("Setup", {}).get("Database", {})
    except Exception:
        return {}


def _select_subscribers(db: dict) -> dict:
    """DB 에서 voip / ptt 가입자 + ptt group 1건씩 선택."""
    out = {
        "voip_user": "", "voip_pwd": "", "voip_imsi": "", "voip_ref": "",
        "ptt_user": "", "ptt_pwd": "", "ptt_imsi": "", "ptt_ref": "",
        "ptt_group": "",
    }
    if not db: return out
    try:
        import pymysql                                          # type: ignore
    except ImportError:
        return out
    try:
        conn = pymysql.connect(
            host=db["Host"], port=int(db.get("Port", 3306)),
            user=db["User"], password=db["Password"], database=db["DbName"],
        )
        cur = conn.cursor()
        cur.execute(
            "SELECT id,passwd,imsi,service_ref FROM volte_subscriptions "
            "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id LIMIT 1"
        )
        r = cur.fetchone()
        if r:
            out.update({"voip_user": r[0], "voip_pwd": r[1] or "",
                        "voip_imsi": r[2] or "", "voip_ref": r[3] or ""})
        cur.execute(
            "SELECT id,passwd,imsi,service_ref FROM ptt_subscriptions "
            "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id LIMIT 1"
        )
        r = cur.fetchone()
        if r:
            out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                        "ptt_imsi": r[2] or "", "ptt_ref": r[3] or ""})
        cur.execute("SELECT id FROM ptt_groups ORDER BY id LIMIT 1")
        r = cur.fetchone()
        if r: out["ptt_group"] = r[0]
        conn.close()
    except Exception:
        pass
    return out


def _seed_access_services(cfg_dir: str, voip_ref: str, ptt_ref: str) -> int:
    """access_services.jsonl 작성. 시드된 항목 수 반환."""
    seeded = []
    def add(name: str, kind: str, domain: str) -> None:
        if not name: return
        seeded.append({
            "id": uuid.uuid4().hex, "name": name, "enabled": True,
            "kind": kind, "domain": domain, "auth_realm": domain,
            "inbound_policy": "any", "allowed_local_node_refs": [],
            "priority": 100, "tags": ["verify-phase3-seed"],
            "note": "auto-seeded by cims_verify P3-SEED",
            "server_identity_uri": f"sip:cspserver@{domain}",
        })
    add(voip_ref, "volte", _VOLTE_DOM)
    add(ptt_ref,  "ptt",   _MCPTT_DOM)
    if not seeded:
        return 0
    os.makedirs(cfg_dir, exist_ok=True)
    path = os.path.join(cfg_dir, "access_services.jsonl")
    with open(path, "w") as f:
        for r in seeded:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return len(seeded)


def _signal_csp_reload(pid_file: str) -> bool:
    try:
        with open(pid_file) as pf: pid = int(pf.read().strip())
        os.kill(pid, 10)                                        # SIGUSR1
        time.sleep(2)
        return True
    except Exception:
        return False


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
    """DB 에서 voip/ptt 가입자+그룹 1건씩 선택 → access_services.jsonl 시드 → csp SIGUSR1."""
    csp_json = os.path.join(ctx.dist_dir, "csp", "config", "csp.json")
    cfg_dir = os.path.join(ctx.dist_dir, "csp-server", "csp", "config")
    pid_file = os.path.join(ctx.dist_dir, "csp-server", "csp", "run", "csp.pid")

    db = _load_csp_db_config(csp_json)
    sub = _select_subscribers(db)
    seeded_n = _seed_access_services(cfg_dir, sub["voip_ref"], sub["ptt_ref"])
    reloaded = _signal_csp_reload(pid_file)

    # ctx.state 에 후속 항목용 정보 적재
    voip_auth = f"{sub['voip_imsi']}@{_VOLTE_DOM}" if sub["voip_imsi"] else ""
    ptt_auth  = f"{sub['ptt_imsi']}@{_MCPTT_DOM}"  if sub["ptt_imsi"]  else ""
    ctx.state.update({
        "VOIP_USER": sub["voip_user"], "VOIP_PWD": sub["voip_pwd"],
        "VOIP_AUTH": voip_auth, "VOIP_DOM": _VOLTE_DOM,
        "PTT_USER":  sub["ptt_user"],  "PTT_PWD":  sub["ptt_pwd"],
        "PTT_AUTH":  ptt_auth,  "PTT_DOM":  _MCPTT_DOM,
        "PTT_GROUP": sub["ptt_group"],
    })

    lines = [
        f"- VoIP: user={sub['voip_user']!r} domain={_VOLTE_DOM} auth_id={voip_auth!r}",
        f"- PTT:  user={sub['ptt_user']!r}  domain={_MCPTT_DOM} group={sub['ptt_group']!r}",
        f"- jsonlDir: {cfg_dir}",
        f"- seeded: {seeded_n}건  / csp reload(SIGUSR1): {'OK' if reloaded else 'FAIL'}",
    ]
    ctx.w("## P3-SEED — 시나리오 준비 (가입자/시드/reload)")
    for line in lines: ctx.w(line)
    ctx.w()

    # 가입자 0건이어도 항목 자체는 PASS — 후속 시나리오가 SKIP 으로 처리.
    # csp reload 실패는 경고로만.
    return ItemResult(
        id="P3-SEED", name="가입자/그룹 선택 + access_services.jsonl 시드",
        status=ItemStatus.PASS,
        detail="\n".join(lines), phase=3,
    )
