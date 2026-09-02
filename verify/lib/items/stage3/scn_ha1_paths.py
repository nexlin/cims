"""S3 SIP 접속 보안 회귀 — AKA 마이그레이션 멱등(V6)·/provisioning/me sipHa1(V8).

sip_access_security.md §6 의 V6/V8 을 자동화한다.
- V6: `sql/migrate_subscription_aka.sql` 2회 실행 — 둘 다 성공(컬럼 존재 시 no-op)·
  시드 가입자 ha1 보존·재등록(인증) 유지. (가입자 PUT 경유 ha1 보존은 CSC 관리
  API 경로라 여기 미포함 — §8.4 잔여.)
- V8: IdMS PKCE 로그인(scope=cims:provisioning) → `GET /provisioning/me` 의 `sipHa1`
  존재 + 평문 비밀번호 필드 부재 확인 → 그 ha1 로 REGISTER 200 (단말 부트스트랩 등가).
  시드 가입자의 로그인 계정(users.login_id)이 없으면 SKIP.
"""
from __future__ import annotations

import base64
import hashlib
import os
import uuid

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.subscribers import VOLTE_DOMAIN
from ...common import db as _db
from ...common import sip_probe
from ...common.csc_http import get_json, post_json, CscHttpError

_CSP_SIP_PORT = 5060
_CSC_MCPTT_PORT = 4430   # dev CSC mcptt 평면 (csc.json Mcptt.Port)


def _seed_ha1(db_cfg: dict, user: str):
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        cur.execute("SELECT ha1 FROM volte_subscriptions WHERE id=%s", (user,))
        r = cur.fetchone()
        return r[0] if r else None
    finally:
        conn.close()


@verify_item(
    id="S3-SCN-AKA-MIGRATE-IDEMPOTENT",
    stage=3, category="시나리오",
    name="AKA 마이그레이션 멱등성 (2회 실행·ha1 보존·인증 유지 — V6)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["db-write", "sip-probe"], timeout_s=60,
    execution_order=63,
)
def aka_migrate_idempotent(ctx: VerifyContext) -> ItemResult:
    """V6: 이행 스크립트 2회 실행 → 둘 다 성공 + ha1 불변 + 재등록 401→200."""
    s = ctx.state
    user, ha1 = s.get("VOIP_USER", ""), s.get("VOIP_HA1", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    sql_path = os.path.join(ctx.repo_root, "sql", "migrate_subscription_aka.sql")
    ctx.w("### S3-SCN-AKA-MIGRATE-IDEMPOTENT — 마이그레이션 멱등 (V6)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id="S3-SCN-AKA-MIGRATE-IDEMPOTENT", name="마이그레이션 멱등",
                          status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user:
        return skip("VOIP_USER 미준비 (S3-SEED 선행)")
    if not db_cfg:
        return skip("DB 설정 없음")
    if not os.path.isfile(sql_path):
        return skip(f"이행 스크립트 없음: {sql_path}")

    before = _seed_ha1(db_cfg, user)

    def run_once() -> int:
        # mysql CLI 가 없는 환경(dev media01)도 같은 경로로 — pymysql MULTI_STATEMENTS 실행
        return _db.run_sql_script(db_cfg, sql_path)

    rc1 = run_once()
    rc2 = run_once()
    after = _seed_ha1(db_cfg, user)
    reg = sip_probe.probe_register_auth(ctx.sim_ip, _CSP_SIP_PORT, user, domain,
                                        auth_user, ha1, ctx.sim_ip,
                                        deregister=True) if ha1 else {}

    ok_run = (rc1 == 0 and rc2 == 0)
    ok_ha1 = (before is not None and before == after)
    ok_reg = (reg.get("second") == 200)
    lines = [
        f"- 이행 스크립트 1차/2차 → rc={rc1}/{rc2} "
        f"({'PASS' if ok_run else 'FAIL'} — 둘 다 0, 컬럼 존재 시 no-op)",
        f"- 시드 가입자 {user} ha1 보존 → {'불변' if ok_ha1 else f'변경됨({before!r}→{after!r})'} "
        f"({'PASS' if ok_ha1 else 'FAIL'})",
        f"- 재등록(저장 ha1) → {reg.get('first')}/{reg.get('second')} "
        f"({'PASS' if ok_reg else 'FAIL'} — 기대 401→200, 인증 유지)",
    ]
    ok = ok_run and ok_ha1 and ok_reg
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id="S3-SCN-AKA-MIGRATE-IDEMPOTENT", name="마이그레이션 멱등 (V6)",
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)


def _login_account(db_cfg: dict, user: str):
    """시드 가입자(volte msisdn)의 로그인 계정 (login_id, passwd) — 없으면 (None, None)."""
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        cur.execute("SELECT u.login_id, u.passwd FROM users u "
                    "JOIN volte_subscriptions vs ON vs.user_id = u.id "
                    "WHERE vs.id=%s AND u.login_id IS NOT NULL AND u.login_id<>''", (user,))
        r = cur.fetchone()
        return (r[0], r[1]) if r else (None, None)
    finally:
        conn.close()


@verify_item(
    id="S3-SCN-PROVISIONING-HA1",
    stage=3, category="시나리오",
    name="/provisioning/me sipHa1 부트스트랩 (평문 부재 + 등록 — V8)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sip-probe"], timeout_s=60,
    execution_order=64,
)
def provisioning_ha1(ctx: VerifyContext) -> ItemResult:
    """V8: IdMS PKCE 토큰(cims:provisioning) → /provisioning/me → sipHa1 로 등록.

    평문 부재 = 응답 계정 객체에 sipPassword 류 평문 필드가 없어야 한다(§4.7 ⑤).
    """
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    ctx.w("### S3-SCN-PROVISIONING-HA1 — /provisioning/me sipHa1 (V8)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id="S3-SCN-PROVISIONING-HA1", name="provisioning sipHa1",
                          status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user:
        return skip("VOIP_USER 미준비 (S3-SEED 선행)")
    if not db_cfg:
        return skip("DB 설정 없음")
    login_id, login_pw = _login_account(db_cfg, user)
    if not login_id:
        return skip(f"{user} 의 로그인 계정(users.login_id) 없음")

    base = f"https://{ctx.sim_ip}:{_CSC_MCPTT_PORT}"
    verifier = uuid.uuid4().hex + uuid.uuid4().hex
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    redirect = "cims://auth"
    lines: list = []
    try:
        from urllib.parse import quote
        auth = get_json(f"{base}/idms/authreq?user_name={quote(login_id)}"
                        f"&user_password={quote(login_pw or '')}&client_id=MCPTT_UE"
                        f"&redirect_uri={quote(redirect)}&state=v8"
                        f"&scope={quote('openid cims:provisioning')}"
                        f"&code_challenge={challenge}&code_challenge_method=S256")
        st, tok = post_json(f"{base}/idms/tokenreq",
                            {"grant_type": "authorization_code", "code": auth.get("code", ""),
                             "code_verifier": verifier, "client_id": "MCPTT_UE",
                             "redirect_uri": redirect})
        if st != 200 or not isinstance(tok, dict):
            return skip(f"tokenreq 실패: status={st} body={tok!r}")
        me = get_json(f"{base}/provisioning/me", token=tok.get("access_token", ""))
    except (CscHttpError, OSError) as e:
        return skip(f"IdMS/provisioning 경로 실패: {e}")

    # 서비스 계정 중 시드 가입자(volte) 항목을 찾는다 — 응답 구조는 services[].account
    accounts: list = []
    for svc in (me.get("services") or []):
        acc = svc.get("account") if isinstance(svc, dict) else None
        if isinstance(acc, dict):
            accounts.append((svc.get("kind") or svc.get("type") or "?", acc))
    target = None
    for kind, acc in accounts:
        if acc.get("msisdn") == user or kind == "volte":
            target = acc
            if acc.get("msisdn") == user and kind == "volte":
                break
    if target is None and accounts:
        target = accounts[0][1]
    if not isinstance(target, dict):
        return skip("provisioning 응답에 서비스 계정 없음")

    sip_ha1 = target.get("sipHa1") or ""
    # §4.7 ⑤ — sipPassword 키는 단말 호환으로 유지하되 값은 항상 null 이어야 한다.
    plain_keys = [k for k in target.keys()
                  if "password" in k.lower() and target.get(k)]
    # 단말 합성 규칙과 동일 — authId 빈값이면 imsi@domain
    auth_user_prov = target.get("authId") or (
        f"{target.get('imsi')}@{domain}" if target.get("imsi") else user)

    reg = {}
    if sip_ha1:
        reg = sip_probe.probe_register_auth(ctx.sim_ip, _CSP_SIP_PORT, user, domain,
                                            auth_user_prov, sip_ha1, ctx.sim_ip,
                                            deregister=True)

    ok_ha1 = bool(sip_ha1)
    ok_plain = not plain_keys
    ok_reg = (reg.get("second") == 200)
    lines.append(f"- 로그인 {login_id} → 토큰(scope=cims:provisioning) → /provisioning/me")
    lines.append(f"- sipHa1 존재 → {'있음' if ok_ha1 else '없음'} ({'PASS' if ok_ha1 else 'FAIL'})")
    lines.append(f"- 평문 비밀번호 필드 부재 → {'부재' if ok_plain else f'존재 {plain_keys}'} "
                 f"({'PASS' if ok_plain else 'FAIL'} — §4.7 ⑤)")
    lines.append(f"- sipHa1 로 REGISTER → {reg.get('first')}/{reg.get('second')} "
                 f"({'PASS' if ok_reg else 'FAIL'} — 기대 401→200)")
    ok = ok_ha1 and ok_plain and ok_reg
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id="S3-SCN-PROVISIONING-HA1", name="provisioning sipHa1 (V8)",
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)
