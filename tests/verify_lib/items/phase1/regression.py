"""Phase 1 §7 — 회귀 시나리오 (VoIP / PTT).

Phase 3 와 달리 build/dist 직접 기동본(Test-CSC 4421 / Test-CSP 5060 / Test-CMP 9000)
대상. access_services.jsonl 위치 + PID 파일이 다르다 (Phase 1: $DIST_DIR/config/,
Phase 3: $DIST_DIR/csp-server/csp/config/).

각 시나리오는 cspsim 호출 + 녹취 파일 증분(delta>=1) 으로 PASS 판정.
"""
from __future__ import annotations

import json
import os
import subprocess
import time
import uuid
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext, sanitized_env


_VOLTE_DOM = "ims.mnc033.mcc450.3gppnetwork.org"
_MCPTT_DOM = "ptt.mnc033.mcc450.3gppnetwork.org"


# ─────────────────────────────────────────────────────────────
# 시드 (P1-SEED) — DB 조회 + access_services.jsonl 작성 + csp SIGUSR1
# ─────────────────────────────────────────────────────────────
def _select_subscribers(db: dict) -> dict:
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
    seeded = []
    def add(name: str, kind: str, domain: str) -> None:
        if not name: return
        seeded.append({
            "id": uuid.uuid4().hex, "name": name, "enabled": True,
            "kind": kind, "domain": domain, "auth_realm": domain,
            "inbound_policy": "any", "allowed_local_node_refs": [],
            "priority": 100, "tags": ["verify-phase1-seed"],
            "note": "auto-seeded by cims_verify P1-SEED",
            "server_identity_uri": f"sip:cspserver@{domain}",
        })
    add(voip_ref, "volte", _VOLTE_DOM)
    add(ptt_ref,  "ptt",   _MCPTT_DOM)
    if not seeded: return 0
    os.makedirs(cfg_dir, exist_ok=True)
    path = os.path.join(cfg_dir, "access_services.jsonl")
    with open(path, "w") as f:
        for r in seeded:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return len(seeded)


def _signal_csp(pid_file: str) -> bool:
    try:
        with open(pid_file) as pf: pid = int(pf.read().strip())
        os.kill(pid, 10)
        time.sleep(2)
        return True
    except Exception:
        return False


@verify_item(
    id="P1-SEED",
    phase=1, category="환경",
    name="가입자/그룹 선택 + access_services.jsonl 시드 + csp reload",
    depends_on=["P1-START"],
    presets=["phase1-full"],
    side_effects=["fs-write", "service-signal"], timeout_s=30,
)
def seed(ctx: VerifyContext) -> ItemResult:
    """Phase 1 회귀용 가입자 정보를 ctx.state 에 적재 + access_services.jsonl 시드."""
    csp_json = os.path.join(ctx.dist_dir, "csp", "config", "csp.json")
    cfg_dir = os.path.join(ctx.dist_dir, "config")
    pid_file = os.path.join(ctx.dist_dir, "run", "csp.pid")

    db = {}
    try:
        with open(csp_json) as f:
            db = json.load(f).get("Setup", {}).get("Database", {})
    except Exception:
        pass
    sub = _select_subscribers(db)
    seeded_n = _seed_access_services(cfg_dir, sub["voip_ref"], sub["ptt_ref"])
    reloaded = _signal_csp(pid_file)

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
    ctx.w("## P1-SEED — 시나리오 준비")
    for line in lines: ctx.w(line)
    ctx.w()
    return ItemResult(
        id="P1-SEED", name="가입자/그룹 선택 + access_services.jsonl 시드",
        status=ItemStatus.PASS, detail="\n".join(lines), phase=1,
    )


# ─────────────────────────────────────────────────────────────
# 회귀 시나리오 — VoIP 2자, PTT 그룹 5인
# ─────────────────────────────────────────────────────────────
def _count_recordings(dist_dir: str) -> int:
    return len(glob(os.path.join(dist_dir, "ext_mnt", "service_log",
                                 "**", "seg_*.rtp"), recursive=True))


def _run_cspsim(ctx: VerifyContext, sim_args: list, timeout: int = 120) -> tuple:
    cims_sh = os.path.join(ctx.repo_root, "cims.sh")
    cmd = ["/bin/bash", cims_sh, "sim"] + sim_args
    try:
        proc = subprocess.run(
            cmd, cwd=ctx.repo_root, env=sanitized_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, text=True,
        )
        out = proc.stdout or ""
        return (proc.returncode, "\n".join(out.splitlines()[-30:]))
    except subprocess.TimeoutExpired:
        return (-1, "(timeout)")
    except Exception as e:
        return (-2, f"({type(e).__name__}: {e})")


def _scenario(ctx: VerifyContext, item_id: str, title: str,
              sim_args: list, prereq_keys: list) -> ItemResult:
    missing = [k for k in prereq_keys if not ctx.state.get(k)]
    if missing:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail=f"가입자/그룹 미준비: {','.join(missing)}", phase=1,
        )
    rec_before = _count_recordings(ctx.dist_dir)
    rc, tail = _run_cspsim(ctx, sim_args)
    rec_after = _count_recordings(ctx.dist_dir)
    delta = rec_after - rec_before
    ok = delta >= 1
    ctx.w(f"### {item_id} — {title}")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w(f"- {'[PASS]' if ok else '[FAIL]'} 녹취 +{delta} (rc={rc})")
    ctx.w()
    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"녹취 +{delta} (rc={rc})\n{tail[-500:]}", phase=1,
    )


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
        "-domain", s.get("VOIP_DOM", _VOLTE_DOM),
        "-password", s.get("VOIP_PWD", ""),
    ]
    if s.get("VOIP_AUTH"): args += ["-auth_id", s["VOIP_AUTH"]]
    return _scenario(ctx, "P1-REGRESS-VOIP",
                     "VoIP 2자 통화 (B2BUA)", args, ["VOIP_USER"])


@verify_item(
    id="P1-REGRESS-PTT",
    phase=1, category="시나리오",
    name="PTT 그룹 통화 (5인, Test-CSP)",
    depends_on=["P1-SEED"],
    presets=["phase1-full"],
    side_effects=["sim-call"], timeout_s=180,
)
def regress_ptt(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-domain", s.get("PTT_DOM", _MCPTT_DOM),
        "-group", s.get("PTT_GROUP", ""),
    ]
    return _scenario(ctx, "P1-REGRESS-PTT",
                     "PTT 그룹 통화 (5인)", args, ["PTT_USER", "PTT_GROUP"])
