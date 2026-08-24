"""S3 SIP 접속 보안 게이트 회귀 — 채널 정책(P0)·realm 대조(P1-a).

sip_access_security.md §6 의 V1/V2/V7 을 원시 SIP 프로브로 자동화한다. cspsim 은
call 시나리오에서 `-transport` 를 무시하고 INVITE 를 UDP 로만 보내므로(§7 잔여),
"게이트가 인증보다 먼저 403" 판정은 cspsim 통계로 재현되지 않는다 — 이 항목은
verify/lib/common/sip_probe 로 REGISTER/MESSAGE 를 직접 조립해 첫 최종 응답 코드를
본다(INVITE 는 dev TestEnvOpenTermination 단락·무SDP 488 로 게이트에 못 닿아 MESSAGE 사용).

정책 플립은 DB(sip_transport)를 바꾸고 CSP 4421 USER_CHANGED 통지로 캐시를 즉시
갱신(ReloadFromDb)한 뒤, 종료 시 원값으로 되돌린다(자기복원 — 공유 DB 안전).
"""
from __future__ import annotations

import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.subscribers import (
    VOLTE_DOMAIN, get_transport_policy, set_transport_policy,
)
from ...common.csp_notify import notify_csp_event
from ...common import sip_probe

_CSP_SIP_PORT = 5060  # dev access-udp 리스너 (build/dist/config/local_nodes.jsonl)


def _notify_user(csp_ip: str, user: str, action: str = "PUT") -> None:
    # CscInterface 는 uri 에서 tel: 만 벗기고 '+' 는 유지 → tel:+82... 로 보낸다.
    # 4421 은 primary local_node bind_ip 에 바인딩되므로(CscInterface.cpp — 특정 IP,
    # 0.0.0.0 아님) 반드시 CSP 접속 IP 로 보낸다(127.0.0.1 은 닿지 않는다).
    notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action=action, ip=csp_ip)


@verify_item(
    id="S3-SCN-CHANNEL-POLICY",
    stage=3, category="시나리오",
    name="채널 정책 게이트 (TLS 강제 신원의 UDP 요청 403 — V1/V2)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["db-write", "service-signal", "sip-probe"], timeout_s=40,
    execution_order=60,
)
def channel_policy(ctx: VerifyContext) -> ItemResult:
    """V1: TLS 강제 가입자의 UDP REGISTER → 403 (챌린지 앞).
    V2: 같은 신원의 인증 없는 UDP MESSAGE → 403 (인증 앞, 401 아님).

    대조: 정책 부여 전(원 정책, 통상 NULL) 같은 프로브는 401 이어야 한다 — 403 이
    게이트 때문임을 증명한다(가입자 자체가 막힌 게 아니라).
    """
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    ctx.w("### S3-SCN-CHANNEL-POLICY — 채널 정책 게이트 (V1/V2)")
    if not user:
        ctx.w("- [SKIP] VOIP_USER 미준비 (S3-SEED 선행)")
        ctx.w()
        return ItemResult(id="S3-SCN-CHANNEL-POLICY", name="채널 정책 게이트",
                          status=ItemStatus.SKIP, detail="VOIP_USER 미준비", stage=3)
    if not db_cfg:
        ctx.w("- [SKIP] DB 설정 없음 (정책 플립 불가)")
        ctx.w()
        return ItemResult(id="S3-SCN-CHANNEL-POLICY", name="채널 정책 게이트",
                          status=ItemStatus.SKIP, detail="DB 설정 없음", stage=3)

    orig = get_transport_policy(db_cfg, "volte_subscriptions", user)
    if orig is None:
        ctx.w(f"- [SKIP] {user} 가 volte_subscriptions 에 없음")
        ctx.w()
        return ItemResult(id="S3-SCN-CHANNEL-POLICY", name="채널 정책 게이트",
                          status=ItemStatus.SKIP, detail=f"{user} 미존재", stage=3)

    lines: list = []
    c_reg = c_msg = v1 = v2 = 0
    try:
        # 대조: 정책 부여 전 프로브 (통상 NULL → 401 챌린지)
        c_reg = sip_probe.probe_register(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
        c_msg = sip_probe.probe_nonregister(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)

        set_transport_policy(db_cfg, "volte_subscriptions", user, "TLS")
        _notify_user(ctx.sim_ip, user)
        time.sleep(1.0)  # ReloadFromDb 반영 대기

        v1 = sip_probe.probe_register(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
        v2 = sip_probe.probe_nonregister(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
    finally:
        # 원값 복원 (NULL 이었으면 NULL 로) + 캐시 갱신
        set_transport_policy(db_cfg, "volte_subscriptions", user,
                             None if orig in ("", None) else orig)
        _notify_user(ctx.sim_ip, user)
        time.sleep(1.0)  # 복원 반영(ReloadFromDb) settle — 뒤 항목/재실행 보호

    ctrl_ok = (c_reg == 401 and c_msg == 401)
    v1_ok = (v1 == 403)
    v2_ok = (v2 == 403)
    lines.append(f"- 대상 가입자: {user} (원 정책 {orig!r} → TLS 강제 후 UDP 프로브 → 복원)")
    lines.append(f"- 대조 REGISTER/MESSAGE (정책 전) → {c_reg or '무응답'}/{c_msg or '무응답'} "
                 f"({'PASS' if ctrl_ok else 'FAIL'} — 기대 401/401, 403 이 게이트임을 증명)")
    lines.append(f"- V1 REGISTER/UDP → {v1 or '무응답'} "
                 f"({'PASS' if v1_ok else 'FAIL'} — 기대 403, 챌린지 401 이면 게이트 미작동)")
    lines.append(f"- V2 MESSAGE/UDP(무인증) → {v2 or '무응답'} "
                 f"({'PASS' if v2_ok else 'FAIL'} — 기대 403, 인증보다 먼저)")
    ok = ctrl_ok and v1_ok and v2_ok
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(
        id="S3-SCN-CHANNEL-POLICY", name="채널 정책 게이트 (V1/V2)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="\n".join(lines), stage=3,
    )


@verify_item(
    id="S3-SCN-REALM-MISMATCH",
    stage=3, category="시나리오",
    name="realm 대조 재챌린지 (틀린 realm Authorization → 401 — V7)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sip-probe"], timeout_s=20,
    execution_order=59,
)
def realm_mismatch(ctx: VerifyContext) -> ItemResult:
    """V7: 서버 nonce 로 **틀린 realm** Digest 를 만들어 REGISTER → 401 재챌린지.

    저장 ha1 은 서버 realm 으로만 검증 가능하므로(§4.3), realm 불일치는 response
    검증 전에 잡혀 401 이 되어야 한다(200/403 이 아니다).
    """
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    ha1 = s.get("VOIP_HA1", "")
    ctx.w("### S3-SCN-REALM-MISMATCH — realm 대조 재챌린지 (V7)")
    if not user:
        ctx.w("- [SKIP] VOIP_USER 미준비 (S3-SEED 선행)")
        ctx.w()
        return ItemResult(id="S3-SCN-REALM-MISMATCH", name="realm 대조 재챌린지",
                          status=ItemStatus.SKIP, detail="VOIP_USER 미준비", stage=3)

    res = sip_probe.probe_register_wrong_realm(
        ctx.sim_ip, _CSP_SIP_PORT, user, domain,
        auth_user=auth_user, ha1_hex=ha1, password="", local_ip=ctx.sim_ip)

    first_ok = (res["first"] == 401)
    second_ok = (res["second"] == 401)
    lines = [
        f"- 대상 가입자: {user} (auth_id={auth_user!r}, 서버 realm={res['server_realm']!r})",
        f"- 1차 REGISTER(무인증) → {res['first'] or '무응답'} "
        f"({'PASS' if first_ok else 'FAIL'} — 기대 401 챌린지)",
        f"- 2차 REGISTER(틀린 realm Digest) → {res['second'] or '무응답'} "
        f"({'PASS' if second_ok else 'FAIL'} — 기대 401 재챌린지)",
    ]
    ok = first_ok and second_ok
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(
        id="S3-SCN-REALM-MISMATCH", name="realm 대조 재챌린지 (V7)",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="\n".join(lines), stage=3,
    )
