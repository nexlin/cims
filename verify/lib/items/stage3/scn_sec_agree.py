"""S3 SIP 접속 보안 — RFC 3329 sec-agree 협상(P2) 회귀.

sip_access_security.md §8.1 의 V9~V12 를 TLS 위 원시 SIP 프로브(sip_probe.SecAgreeTlsSession)로
자동화한다. 한 TLS 연결에서 초기 REGISTER(Security-Client/Require) → 401(Security-Server) →
Security-Verify 를 실은 재-REGISTER 까지 수행하고, 협상 등록이 살아있는 동안 같은 신원의 UDP
요청이 채널 정책 게이트(§3)에 걸리는지(협상 결과의 게이트 합류)까지 본 뒤 등록을 해제한다.

421(정책상 협상 필수)은 `Setup.SecAgree.Require=true` 환경에서만 성립하므로 여기서는 다루지
않는다 — 설정과 무관한 494 계약만 검증한다.
"""
from __future__ import annotations

import json
import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.subscribers import VOLTE_DOMAIN
from ...common import sip_probe

_CSP_SIP_PORT = 5060   # dev access-udp 리스너
_CSP_TLS_PORT = 5061   # dev access-tls 리스너 (local_nodes.jsonl 에서 읽고, 없으면 이 값)


def _tls_port(dist_dir: str) -> int:
    path = os.path.join(dist_dir, "config", "local_nodes.jsonl")
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                n = json.loads(line)
                if n.get("enabled", True) and str(n.get("protocol", "")).upper() == "TLS":
                    return int(n.get("bind_port") or _CSP_TLS_PORT)
    except (OSError, ValueError):
        pass
    return _CSP_TLS_PORT


@verify_item(
    id="S3-SCN-SEC-AGREE",
    stage=3, category="시나리오",
    name="sec-agree 협상 (RFC 3329 — TLS 등록 200 / 변조·제안없음·Verify 생략 494 / 협상 후 UDP 403 — V9~V12)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sip-probe"], timeout_s=40,
    execution_order=58,
)
def sec_agree(ctx: VerifyContext) -> ItemResult:
    """V9 : Security-Client+Require → 401 에 Security-Server, Security-Verify echo → 200
           (Service-Route 에 ;transport=tls). 등록 유지 중 UDP MESSAGE → 403(게이트 합류),
           해제 후 → 401(복원).
    V10: Security-Verify 변조(강등) → 494 + 새 Security-Server.
    V11: Require: sec-agree 만 있고 Security-Client 없음 → 494.
    V12: 협상 후 Security-Verify 생략 → 494.
    """
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    ha1 = s.get("VOIP_HA1", "")
    rid, rname = "S3-SCN-SEC-AGREE", "sec-agree 협상 (V9~V12)"
    ctx.w("### S3-SCN-SEC-AGREE — RFC 3329 sec-agree 협상 (V9~V12)")
    if not user:
        ctx.w("- [SKIP] VOIP_USER 미준비 (S3-SEED 선행)")
        ctx.w()
        return ItemResult(id=rid, name=rname, status=ItemStatus.SKIP, detail="VOIP_USER 미준비", stage=3)
    if not ha1:
        ctx.w("- [SKIP] 자격 없음 (VOIP_HA1)")
        ctx.w()
        return ItemResult(id=rid, name=rname, status=ItemStatus.SKIP, detail="자격 없음", stage=3)

    tls_port = _tls_port(ctx.dist_dir)
    lines: list = [f"- 대상: {user} (TLS {ctx.sim_ip}:{tls_port}, UDP :{_CSP_SIP_PORT})"]
    checks: list = []

    def chk(label: str, ok: bool, got: str, expect: str) -> None:
        checks.append(ok)
        lines.append(f"- {label} → {got} ({'PASS' if ok else 'FAIL'} — 기대 {expect})")

    # V9 정상 협상 + 게이트 합류 (세션은 등록 해제까지 유지)
    sess = None
    try:
        sess = sip_probe.SecAgreeTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
        r = sess.register(user, domain, auth_user, ha1, "")
        chk("V9 초기 REGISTER(Security-Client/Require)", r["first"] == 401 and bool(r["security_server"]),
            f"{r['first'] or '무응답'} Security-Server={r['security_server'] or '없음'}", "401 + Security-Server")
        chk("V9 재-REGISTER(Security-Verify echo, TLS)", r["second"] == 200,
            f"{r['second'] or '무응답'} Service-Route={r['service_route'] or '없음'}", "200")
        chk("V9 Service-Route ;transport=tls", "transport=tls" in r["service_route"].lower(),
            r["service_route"] or "없음", ";transport=tls 포함")
        gate = sip_probe.probe_nonregister(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
        chk("V9 협상 등록 유지 중 UDP MESSAGE", gate == 403, f"{gate or '무응답'}", "403 (협상 결과의 게이트 합류)")
        u = sess.register(user, domain, auth_user, ha1, "", expires=0)
        chk("V9 등록 해제(Expires 0, Security-Verify)", u["second"] == 200, f"{u['second'] or '무응답'}", "200")
        after = sip_probe.probe_nonregister(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
        chk("V9 해제 후 UDP MESSAGE", after == 401, f"{after or '무응답'}", "401 (게이트 복원)")
    except OSError as e:
        lines.append(f"- [FAIL] TLS 연결 실패: {e}")
        checks.append(False)
    finally:
        if sess:
            sess.close()

    # V10 변조 / V11 제안 없음 / V12 Verify 생략 — 각각 새 연결, 등록이 성립하지 않으므로 잔여 없음
    for label, kwargs, key, expect in (
        ("V10 Security-Verify 변조(강등)", {"verify": "tls;q=0.9"}, "second", 494),
        ("V11 Require: sec-agree 만(Security-Client 없음)", {"security_client": None}, "first", 494),
        ("V12 협상 후 Security-Verify 생략", {"verify": None}, "second", 494),
    ):
        try:
            sx = sip_probe.SecAgreeTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
            try:
                rx = sx.register(user, domain, auth_user, ha1, "", **kwargs)
            finally:
                sx.close()
            got = rx[key]
            ss = rx["second_security_server"] if key == "second" else rx["security_server"]
            chk(label, got == expect and bool(ss), f"{got or '무응답'} Security-Server={ss or '없음'}",
                f"{expect} + 새 Security-Server")
        except OSError as e:
            lines.append(f"- [FAIL] {label}: TLS 연결 실패: {e}")
            checks.append(False)

    ok = all(checks) and bool(checks)
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id=rid, name=rname, status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)
