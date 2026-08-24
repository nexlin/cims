"""S3 SIP 접속 보안 — IMS AKA + IPsec (P4, sip_access_security.md §8.3) 회귀.

S3-SCN-IPSEC (V19·V20 — 커널 특권 불필요): 초기 REGISTER 의 `Security-Client: ipsec-3gpp;…` 제안에
서버가 무엇을 제시하는지 본다.
  · V20 Digest 가입자의 제안 → `Security-Server` 에 `tls` 만 (IPsec 키는 AKA 의 CK/IK).
  · V19 AKA 가입자의 제안 + NAT 흔적(top Via sent-by 가 실소스와 다름) → `tls` 만 (Annex M 미지원 —
    협상 단계에서 가른다). 같은 가입자의 제안 없는 UDP REGISTER 는 종전대로 403(게이트).
    환경에 IPSEC Local Node + CSP `ipsec: available` 이 있으면 NAT 흔적 없는 제안이 `ipsec-3gpp` 를
    받는 양성 대조를 함께 본다. AKA 컬럼(migrate_subscription_aka.sql)·AuC 설정이 없으면 V19 는 생략.

S3-SCN-IPSEC-LIVE (V21~V23 — cspsim `-ipsec`, CAP_NET_ADMIN 필요): 전제(AKA 환경·IPSEC Local Node·
cspsim capability·CSP ipsec 가용)가 없으면 SKIP.
"""
from __future__ import annotations

import glob
import json
import os
import subprocess
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.subscribers import VOLTE_DOMAIN
from ...common.csp_notify import notify_csp_event
from ...common import sip_probe
from ...common.cspsim import run_cspsim
from .scn_aka import _csc_cfg, _keystore, _read_row, _write_row, _K, _OPC

_CSP_SIP_PORT = 5060
_OFFER = "ipsec-3gpp;alg=hmac-sha-1-96;ealg=aes-cbc;spi-c=1001;spi-s=1002;port-c=7001;port-s=7002"


def _ipsec_local_node(dist_dir: str) -> dict:
    try:
        with open(os.path.join(dist_dir, "config", "local_nodes.jsonl"), encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                row = json.loads(line)
                if str(row.get("protocol", "")).upper() == "IPSEC" and row.get("enabled", True):
                    return row
    except Exception:
        pass
    return {}


def _csp_ipsec_available(dist_dir: str) -> bool:
    """가장 최근 CSP 로그에서 기동 자기점검 결과를 본다 — `ipsec: available` 이 마지막 판정이어야 한다."""
    logs = sorted(glob.glob(os.path.join(dist_dir, "csp", "log", "csp_*.log")), key=os.path.getmtime)
    for path in reversed(logs[-3:]):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                verdict = None
                for line in f:
                    if "ipsec: available" in line:
                        verdict = True
                    elif "ipsec-3gpp is not offered" in line:
                        verdict = False
                if verdict is not None:
                    return verdict
        except Exception:
            continue
    return False


def _has_cap(path: str) -> bool:
    try:
        r = subprocess.run(["getcap", path], capture_output=True, text=True, timeout=5)
        return "cap_net_admin" in (r.stdout or "")
    except Exception:
        return False


def _aka_env(ctx: VerifyContext, user: str):
    """(db_cfg, keystore, kek) 또는 (None, reason)"""
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    if not db_cfg:
        return None, "DB 설정 없음"
    csc = _csc_cfg(ctx.dist_dir)
    kek_raw = str((csc.get("AuC") or {}).get("Kek") or "")
    if not kek_raw or not str((csc.get("InternalApi") or {}).get("Token") or ""):
        return None, "csc.json AuC.Kek / InternalApi.Token 미설정"
    try:
        if _read_row(db_cfg, user) is None:
            return None, f"{user} 가 volte_subscriptions 에 없음"
    except Exception as e:
        return None, f"AKA 컬럼 없음 — migrate_subscription_aka.sql 미적용 ({e})"
    ks = _keystore(ctx.repo_root)
    return (db_cfg, ks, ks.normalize_kek(kek_raw)), ""


@verify_item(
    id="S3-SCN-IPSEC",
    stage=3, category="시나리오",
    name="IMS AKA+IPsec 협상 게이트 (Digest 제안 → tls / NAT 감지 → tls / 제안 없는 AKA UDP 403 — V19·V20)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sip-probe", "db-write"], timeout_s=60,
    execution_order=55,
)
def ipsec_offer(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    rid, rname = "S3-SCN-IPSEC", "IMS AKA+IPsec 협상 게이트 (V19·V20)"
    ctx.w("### S3-SCN-IPSEC — IMS AKA+IPsec 협상 게이트 (V19·V20)")
    if not user:
        ctx.w("- [SKIP] VOIP_USER 미준비 (S3-SEED 선행)")
        ctx.w()
        return ItemResult(id=rid, name=rname, status=ItemStatus.SKIP, detail="VOIP_USER 미준비", stage=3)

    lines: list = [f"- 대상: {user} (UDP {ctx.sim_ip}:{_CSP_SIP_PORT})"]
    checks: list = []

    def chk(label: str, ok: bool, got: str, expect: str) -> None:
        checks.append(ok)
        lines.append(f"- {label} → {got} ({'PASS' if ok else 'FAIL'} — 기대 {expect})")

    def offer(via_host: str = "", via_port: int = 0) -> tuple:
        return sip_probe.probe_register_offer(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip, _OFFER,
                                             via_host=via_host, via_port=via_port)

    # V20 — Digest 가입자: 제안은 받되 tls 만
    code, server = offer()
    chk("V20 Digest 가입자의 ipsec-3gpp 제안", code == 401 and server != "" and "ipsec-3gpp" not in server,
        f"{code} Security-Server='{server}'", "401 + tls 만")

    # V19 — AKA 가입자 + NAT 흔적
    env, why = _aka_env(ctx, user)
    if env is None:
        lines.append(f"- V19 생략: {why}")
    else:
        db_cfg, ks, kek = env
        orig = _read_row(db_cfg, user)
        try:
            _write_row(db_cfg, user, ("aka", ks.encrypt(kek, bytes.fromhex(_K)),
                                      ks.encrypt(kek, bytes.fromhex(_OPC)), 0, "8000"))
            notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
            time.sleep(1.0)
            code, server = offer(via_host="10.255.0.1", via_port=5060)
            chk("V19a AKA 가입자 제안 + NAT 흔적(sent-by 10.255.0.1)", code == 401 and server != "" and
                "ipsec-3gpp" not in server, f"{code} Security-Server='{server}'", "401 + tls 만")
            code = sip_probe.probe_register(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
            chk("V19b 같은 가입자의 제안 없는 UDP REGISTER", code == 403, str(code), "403 (게이트)")
            node = _ipsec_local_node(ctx.dist_dir)
            if node and _csp_ipsec_available(ctx.dist_dir):
                code, server = offer()
                chk("V19c NAT 흔적 없는 제안 (양성 대조)", code == 401 and server.startswith("ipsec-3gpp") and
                    f"port-s={node.get('bind_port')}" in server, f"{code} Security-Server='{server}'",
                    "401 + ipsec-3gpp 첫 항목 (port-s=port_ps)")
            else:
                lines.append("- V19c 양성 대조 생략: IPSEC Local Node 또는 CSP ipsec 가용 아님")
        except Exception as e:
            checks.append(False)
            lines.append(f"- [FAIL] 예외: {type(e).__name__}: {e}")
        finally:
            if orig is not None:
                _write_row(db_cfg, user, tuple(orig))
                notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
                time.sleep(1.0)

    ok = all(checks) and bool(checks)
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id=rid, name=rname, status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)


@verify_item(
    id="S3-SCN-IPSEC-LIVE",
    stage=3, category="시나리오",
    name="IMS AKA+IPsec 등록 (cspsim -ipsec: SA 위 200 / Verify 변조 494 / 해제 회수 — V21~V23)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full"],
    side_effects=["sim-call", "db-write"], timeout_s=120,
    execution_order=56,
)
def ipsec_live(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    rid, rname = "S3-SCN-IPSEC-LIVE", "IMS AKA+IPsec 등록 (V21~V23)"
    ctx.w("### S3-SCN-IPSEC-LIVE — IMS AKA+IPsec 등록 (V21~V23)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id=rid, name=rname, status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user:
        return skip("VOIP_USER 미준비 (S3-SEED 선행)")
    env, why = _aka_env(ctx, user)
    if env is None:
        return skip(why)
    node = _ipsec_local_node(ctx.dist_dir)
    if not node:
        return skip("IPSEC Local Node 없음 (local_nodes.jsonl protocol=IPSEC)")
    if not _csp_ipsec_available(ctx.dist_dir):
        return skip("CSP 로그에 'ipsec: available' 없음 — CAP_NET_ADMIN(cims-priv setcap-net-admin) 확인")
    sim_bins = [os.path.join(ctx.dist_dir, "cspsim", "bin", "cspsim"), os.path.join(ctx.repo_root, "build", "bin", "cspsim")]
    if not any(os.path.isfile(b) and _has_cap(b) for b in sim_bins):
        return skip("cspsim 에 cap_net_admin 없음 (setcap cap_net_admin+ep <cspsim>)")

    db_cfg, ks, kek = env
    orig = _read_row(db_cfg, user)
    lines: list = [f"- 대상: {user} port_ps={node.get('bind_port')} port_pc={node.get('client_port')}"]
    checks: list = []

    def chk(label: str, ok: bool, got: str, expect: str) -> None:
        checks.append(ok)
        lines.append(f"- {label} → {got} ({'PASS' if ok else 'FAIL'} — 기대 {expect})")

    base = ["-no-db", "-mode", "volte", "-scenario", "register", "-count", "1", "-ip", ctx.sim_ip,
            "-user", user, "-domain", domain, "-transport", "udp",
            "-aka_k", _K, "-aka_opc", _OPC, "-ipsec"]
    if s.get("VOIP_AUTH"):
        base += ["-auth_id", s["VOIP_AUTH"]]
    try:
        _write_row(db_cfg, user, ("aka", ks.encrypt(kek, bytes.fromhex(_K)),
                                  ks.encrypt(kek, bytes.fromhex(_OPC)), 0, "8000"))
        notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
        time.sleep(1.0)
        rc, out = run_cspsim(ctx.repo_root, base, timeout=60)
        chk("V21 cspsim -ipsec 등록 (SA 설치 → 답안 → 200)", rc == 0 and "registered over SA" in out,
            f"rc={rc}", "rc=0 + 'registered over SA'")
        chk("V23 해제 후 단말 SA 회수", "sa set released" in out, "로그", "'sa set released'")
        rc, out = run_cspsim(ctx.repo_root, base + ["-sec_verify", "tls;q=0.1"], timeout=60)
        chk("V22 Security-Verify 변조", rc != 0 or "registered over SA" not in out, f"rc={rc}",
            "등록 실패 (서버 494)")
    except Exception as e:
        checks.append(False)
        lines.append(f"- [FAIL] 예외: {type(e).__name__}: {e}")
    finally:
        if orig is not None:
            _write_row(db_cfg, user, tuple(orig))
            notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
            time.sleep(1.0)

    ok = all(checks) and bool(checks)
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id=rid, name=rname, status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)
