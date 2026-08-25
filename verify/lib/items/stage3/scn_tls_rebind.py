"""S3 SIP 접속 보안 회귀 — TLS 재접속 재등록(V3)·NULL 정책 혼합 등록(V4).

sip_access_security.md §6 의 V3/V4 를 원시 SIP 프로브로 자동화한다.
- V3: TLS 등록 → 연결을 끊고 **새 연결**에서 재등록 → 정상(재인증 후 바인딩 이동).
- V4: `sip_transport` NULL 가입자의 UDP+TLS 혼합 등록 — 게이트 미작동으로 둘 다 성립.

dev TB 의 local_nodes.jsonl 에는 TLS 접속점이 없으므로 항목이 임시 TLS 리스너를
추가하고(SIGUSR1 hot-add — ListenerManager R6) 종료 시 제거한다(자기복원). 인증서는
dist csc 자가서명 cert 를 빌린다(프로브는 서버 인증서를 검증하지 않는다).
"""
from __future__ import annotations

import json
import os
import socket
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.subscribers import VOLTE_DOMAIN
from ...common.access_services import signal_csp_reload
from ...common import sip_probe

_CSP_SIP_PORT = 5060   # dev access-udp 리스너
_TLS_PORT = 5061       # 임시 TLS 리스너 (dev 관례 — csp.json TlsPort 와 동일 대역)
_TLS_NODE_ID = "verify-tls-rebind"


def _local_nodes_path(dist_dir: str) -> str:
    return os.path.join(dist_dir, "config", "local_nodes.jsonl")


def _find_tls_listener(dist_dir: str) -> int:
    """local_nodes.jsonl 의 enabled TLS 리스너 포트 (없으면 0)."""
    try:
        with open(_local_nodes_path(dist_dir), encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                r = json.loads(line)
                if str(r.get("protocol", "")).upper() == "TLS" and r.get("enabled", True):
                    return int(r.get("bind_port") or 0)
    except Exception:
        pass
    return 0


def _tls_reachable(ip: str, port: int, timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def _ensure_tls_listener(ctx: VerifyContext) -> tuple:
    """(port, added) — 기존 TLS 리스너가 있으면 그대로, 없으면 임시 추가 후 접속 확인."""
    port = _find_tls_listener(ctx.dist_dir)
    if port and _tls_reachable(ctx.sim_ip, port):
        return port, False
    cert = os.path.join(ctx.dist_dir, "csc", "cert", "server.crt")
    key = os.path.join(ctx.dist_dir, "csc", "cert", "server.key")
    if not (os.path.isfile(cert) and os.path.isfile(key)):
        return 0, False
    rec = {"id": _TLS_NODE_ID, "name": _TLS_NODE_ID, "edge": "access",
           "bind_ip": ctx.sim_ip, "bind_port": _TLS_PORT, "protocol": "TLS",
           "enabled": True, "is_primary": False,
           "tls_cert_path": cert, "tls_key_path": key, "tls_verify_peer": False}
    with open(_local_nodes_path(ctx.dist_dir), "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    signal_csp_reload(os.path.join(ctx.dist_dir, "run", "csp.pid"), wait_sec=1.5)
    for _ in range(10):
        if _tls_reachable(ctx.sim_ip, _TLS_PORT):
            return _TLS_PORT, True
        time.sleep(0.5)
    return 0, True   # 추가는 했으나 미개설 — caller 가 정리 후 FAIL 처리


def _remove_tls_listener(ctx: VerifyContext) -> None:
    path = _local_nodes_path(ctx.dist_dir)
    try:
        with open(path, encoding="utf-8") as f:
            lines = [ln for ln in f.read().splitlines() if ln.strip()]
        kept = [ln for ln in lines if json.loads(ln).get("id") != _TLS_NODE_ID]
        if len(kept) != len(lines):
            with open(path, "w", encoding="utf-8") as f:
                f.write("\n".join(kept) + ("\n" if kept else ""))
            signal_csp_reload(os.path.join(ctx.dist_dir, "run", "csp.pid"), wait_sec=1.0)
    except Exception:
        pass


def _tls_register(ctx: VerifyContext, tls_port: int, user: str, domain: str,
                  auth_user: str, ha1: str, expires: int = 60):
    """sec-agree 없는 순수 TLS Digest 등록 — (세션, {first, second})."""
    s = sip_probe.SecAgreeTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
    out = s.register(user, domain, auth_user, ha1, "",
                     security_client=None, require=False, verify=None, expires=expires)
    return s, out


@verify_item(
    id="S3-SCN-TLS-REBIND",
    stage=3, category="시나리오",
    name="TLS 재접속 재등록 (연결 교체 후 바인딩 이동 — V3)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["config-write", "service-signal", "sip-probe"], timeout_s=60,
    execution_order=61,
)
def tls_rebind(ctx: VerifyContext) -> ItemResult:
    """V3: TLS 등록 → 연결 끊김(abrupt close) → 새 연결 재등록 → 200 (바인딩 이동) → 해제."""
    s = ctx.state
    user, ha1 = s.get("VOIP_USER", ""), s.get("VOIP_HA1", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    ctx.w("### S3-SCN-TLS-REBIND — TLS 재접속 재등록 (V3)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id="S3-SCN-TLS-REBIND", name="TLS 재접속 재등록",
                          status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user or not ha1:
        return skip("VOIP_USER/HA1 미준비 (S3-SEED 선행)")

    tls_port, added = _ensure_tls_listener(ctx)
    if not tls_port:
        if added:
            _remove_tls_listener(ctx)
        return skip("TLS 리스너 확보 실패 (임시 추가 후에도 미개설)")

    lines: list = []
    r1 = r2 = d = {}
    try:
        sess1, r1 = _tls_register(ctx, tls_port, user, domain, auth_user, ha1)
        sess1.close()   # abrupt — 해제 없이 연결만 끊는다 (단말 재접속 재현)
        sess2, r2 = _tls_register(ctx, tls_port, user, domain, auth_user, ha1)
        d = sess2.register(user, domain, auth_user, ha1, "",
                           security_client=None, require=False, verify=None, expires=0)
        sess2.close()
    finally:
        if added:
            _remove_tls_listener(ctx)

    ok1 = (r1.get("second") == 200)
    ok2 = (r2.get("second") == 200)
    okd = (d.get("second") == 200)
    lines.append(f"- 대상 가입자: {user} (TLS :{tls_port}{' — 임시 리스너' if added else ''})")
    lines.append(f"- 1차 TLS 등록 → {r1.get('first')}/{r1.get('second')} "
                 f"({'PASS' if ok1 else 'FAIL'} — 기대 401→200)")
    lines.append(f"- 연결 끊고 새 연결 재등록 → {r2.get('first')}/{r2.get('second')} "
                 f"({'PASS' if ok2 else 'FAIL'} — 기대 401→200, 재인증 후 바인딩 이동)")
    lines.append(f"- 해제(Expires:0) → {d.get('second')} ({'PASS' if okd else 'FAIL'})")
    ok = ok1 and ok2 and okd
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id="S3-SCN-TLS-REBIND", name="TLS 재접속 재등록 (V3)",
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)


@verify_item(
    id="S3-SCN-MIXED-TRANSPORT",
    stage=3, category="시나리오",
    name="NULL 정책 UDP+TLS 혼합 등록 회귀 (게이트 미작동 — V4)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["config-write", "service-signal", "sip-probe"], timeout_s=60,
    execution_order=62,
)
def mixed_transport(ctx: VerifyContext) -> ItemResult:
    """V4: `sip_transport` NULL 가입자의 UDP 등록 + TLS 등록 — 둘 다 200 (게이트 미작동)."""
    s = ctx.state
    user, ha1 = s.get("VOIP_USER", ""), s.get("VOIP_HA1", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    ctx.w("### S3-SCN-MIXED-TRANSPORT — NULL 정책 UDP+TLS 혼합 등록 (V4)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id="S3-SCN-MIXED-TRANSPORT", name="혼합 등록 회귀",
                          status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user or not ha1:
        return skip("VOIP_USER/HA1 미준비 (S3-SEED 선행)")

    tls_port, added = _ensure_tls_listener(ctx)
    if not tls_port:
        if added:
            _remove_tls_listener(ctx)
        return skip("TLS 리스너 확보 실패")

    lines: list = []
    udp = {}
    tls = {}
    try:
        # UDP 등록 (해제는 마지막에 — TLS 와 공존을 본다)
        udp = sip_probe.probe_register_auth(ctx.sim_ip, _CSP_SIP_PORT, user, domain,
                                            auth_user, ha1, ctx.sim_ip)
        # TLS 등록 — UDP 바인딩이 살아있는 상태에서 (혼합 공존)
        sess, tls = _tls_register(ctx, tls_port, user, domain, auth_user, ha1)
        sess.register(user, domain, auth_user, ha1, "",
                      security_client=None, require=False, verify=None, expires=0)
        sess.close()
    finally:
        # UDP 바인딩 해제 (자기복원)
        sip_probe.probe_register_auth(ctx.sim_ip, _CSP_SIP_PORT, user, domain,
                                      auth_user, ha1, ctx.sim_ip, expires=0)
        if added:
            _remove_tls_listener(ctx)

    ok_u = (udp.get("second") == 200)
    ok_t = (tls.get("second") == 200)
    lines.append(f"- 대상 가입자: {user} (정책 NULL — 게이트 미작동 기대)")
    lines.append(f"- UDP 등록 → {udp.get('first')}/{udp.get('second')} "
                 f"({'PASS' if ok_u else 'FAIL'} — 기대 401→200)")
    lines.append(f"- TLS 등록 (UDP 유지 중) → {tls.get('first')}/{tls.get('second')} "
                 f"({'PASS' if ok_t else 'FAIL'} — 기대 401→200, 혼합 공존)")
    ok = ok_u and ok_t
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id="S3-SCN-MIXED-TRANSPORT", name="혼합 등록 회귀 (V4)",
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)
