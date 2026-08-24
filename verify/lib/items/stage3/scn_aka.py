"""S3 SIP 접속 보안 — IMS AKA over TLS (P3, sip_access_security.md §8.2) 회귀.

CSC(AuC) 내부 AV API 와 CSP 의 AKAv1-MD5 챌린지/검증/재동기를 원시 프로브로 본다.
  V14 내부 AV API : 토큰 없음 401 / 발급 200(RAND·AUTN·XRES 정합) / AUTS 재동기 200 + DB sqn 갱신
  V15 AKA 등록    : TLS 위 REGISTER → 401 algorithm=AKAv1-MD5 → RES 답안 → 200 (+ Service-Route ;transport=tls)
  V16 MAC 실패    : 틀린 K 로 AUTN MAC 불일치 → 단말이 빈 response 보고 → 403
  V17 SQN 재동기  : 단말 SQN_MS 가 앞서 있음 → auts → 서버 재동기 새 401 → 답안 → 200
  V18 채널 게이트 : AKA 가입자의 UDP REGISTER → 403 (Annex X — TLS 위에서만, sip_transport 와 무관)
전제: DB 에 migrate_subscription_aka.sql 적용 + csc.json AuC.Kek/InternalApi.Token 설정. 미충족이면 SKIP.
가입자의 원 인증 자료(auth_scheme/k_enc/opc_enc/sqn/amf)는 시험 뒤 그대로 복원한다.
"""
from __future__ import annotations

import json
import os
import sys
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.subscribers import VOLTE_DOMAIN
from ...common.csp_notify import notify_csp_event
from ...common import sip_probe
from ...common import csc_http
from .scn_sec_agree import _tls_port

_CSP_SIP_PORT = 5060
# TS 35.208 Test Set 1 — 시험용 K/OPc (실 가입자 키가 아니다)
_K = "465b5ce8b199b49faa5f0a2ee238a6bc"
_OPC = "cd63cb71954a9f4e48a5994e37a02baf"
_BAD_K = "00000000000000000000000000000000"
_AKA_COLS = ("auth_scheme", "k_enc", "opc_enc", "sqn", "amf")


def _csc_cfg(dist_dir: str) -> dict:
    try:
        with open(os.path.join(dist_dir, "csc", "config", "csc.json"), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _keystore(repo_root: str):
    p = os.path.join(repo_root, "csc", "src")
    if p not in sys.path:
        sys.path.insert(0, p)
    from services.auc import keystore  # noqa: WPS433
    return keystore


def _read_row(db_cfg: dict, user: str):
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        cur.execute("SELECT " + ",".join(_AKA_COLS) + " FROM volte_subscriptions WHERE id=%s", (user,))
        return cur.fetchone()
    finally:
        conn.close()


def _write_row(db_cfg: dict, user: str, values: tuple) -> None:
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        cur.execute("UPDATE volte_subscriptions SET " + ",".join(f"{c}=%s" for c in _AKA_COLS) +
                    " WHERE id=%s", tuple(values) + (user,))
    finally:
        conn.close()


@verify_item(
    id="S3-SCN-AKA",
    stage=3, category="시나리오",
    name="IMS AKA over TLS (AV API / AKAv1-MD5 등록 200 / MAC 실패 403 / AUTS 재동기 / UDP 403 — V14~V18)",
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["sip-probe", "db-write"], timeout_s=60,
    execution_order=57,
)
def aka(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    user = s.get("VOIP_USER", "")
    domain = s.get("VOIP_DOM", VOLTE_DOMAIN)
    auth_user = s.get("VOIP_AUTH", "") or user
    rid, rname = "S3-SCN-AKA", "IMS AKA over TLS (V14~V18)"
    ctx.w("### S3-SCN-AKA — IMS AKA over TLS (V14~V18)")

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        ctx.w()
        return ItemResult(id=rid, name=rname, status=ItemStatus.SKIP, detail=reason, stage=3)

    if not user:
        return skip("VOIP_USER 미준비 (S3-SEED 선행)")
    db_cfg = _db.csp_db_config(ctx.dist_dir)
    if not db_cfg:
        return skip("DB 설정 없음")
    csc = _csc_cfg(ctx.dist_dir)
    kek_raw = str((csc.get("AuC") or {}).get("Kek") or "")
    token = str((csc.get("InternalApi") or {}).get("Token") or "")
    if not kek_raw or not token:
        return skip("csc.json AuC.Kek / InternalApi.Token 미설정 (configure 재실행)")
    try:
        orig = _read_row(db_cfg, user)
    except Exception as e:
        return skip(f"AKA 컬럼 없음 — sql/migrate_subscription_aka.sql 미적용 ({e})")
    if orig is None:
        return skip(f"{user} 가 volte_subscriptions 에 없음")

    ks = _keystore(ctx.repo_root)
    kek = ks.normalize_kek(kek_raw)
    csc_port = int((csc.get("Server") or {}).get("Port") or 4421)
    av_url = f"https://{ctx.sim_ip}:{csc_port}/internal/aka/av"
    tls_port = _tls_port(ctx.dist_dir)
    lines: list = [f"- 대상: {user} (CSC AV API {av_url}, TLS {ctx.sim_ip}:{tls_port})"]
    checks: list = []

    def chk(label: str, ok: bool, got: str, expect: str) -> None:
        checks.append(ok)
        lines.append(f"- {label} → {got} ({'PASS' if ok else 'FAIL'} — 기대 {expect})")

    try:
        # AKA 프로비저닝 — CSC 와 같은 보관 형식으로 직접 기록 (키 SoT = CSC keystore)
        _write_row(db_cfg, user, ("aka", ks.encrypt(kek, bytes.fromhex(_K)),
                                  ks.encrypt(kek, bytes.fromhex(_OPC)), 0, "8000"))
        notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
        time.sleep(1.0)

        # V14 — 내부 AV API
        st, _ = csc_http.post_json(av_url, {"msisdn": user}, token=None, timeout=5)
        chk("V14a AV API 토큰 없음", st == 401, str(st), "401")
        st, body = csc_http.post_json(av_url, {"msisdn": user, "service": "volte"}, token=token, timeout=5)
        av = (body or {}).get("av", {}) if isinstance(body, dict) else {}
        ok14 = st == 200 and len(av.get("rand", "")) == 32 and len(av.get("autn", "")) == 32
        if ok14:
            import base64
            nonce = base64.b64encode(bytes.fromhex(av["rand"]) + bytes.fromhex(av["autn"])).decode()
            ans = sip_probe.aka_answer(_K, _OPC, nonce, 0)
            ok14 = ans["mac_ok"] and ans["res"].hex() == av.get("xres") and ans["sqn"] == 1
        chk("V14b AV 발급 (MAC-A/XRES 정합, SQN=1)", ok14, str(st), "200")
        # AUTS 재동기: 단말 SQN_MS=1000 → 서버 sqn 은 1001 로
        auts = sip_probe.aka_auts_hex(_K, _OPC, bytes.fromhex(av.get("rand", "00" * 16)), 1000)
        st, body = csc_http.post_json(av_url, {"msisdn": user, "rand": av.get("rand", ""), "auts": auts},
                                      token=token, timeout=5)
        row = _read_row(db_cfg, user)
        ok14c = st == 200 and isinstance(body, dict) and body.get("resynced") is True and row and int(row[3]) == 1001
        chk("V14c AUTS 재동기 (resynced, sqn=1001)", ok14c, f"{st} sqn={row[3] if row else '?'}", "200/1001")
        st, _ = csc_http.post_json(av_url, {"msisdn": user, "rand": av.get("rand", ""), "auts": "00" * 14},
                                   token=token, timeout=5)
        chk("V14d AUTS 변조", st == 422, str(st), "422")

        # V18 — UDP 는 게이트에서 403 (Annex X)
        c = sip_probe.probe_register(ctx.sim_ip, _CSP_SIP_PORT, user, domain, ctx.sim_ip)
        chk("V18 AKA 가입자 UDP REGISTER", c == 403, str(c or "무응답"), "403")

        # V15 — TLS AKA 등록 200
        sess = sip_probe.AkaTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
        try:
            r = sess.register_aka(user, domain, auth_user, _K, _OPC, sqn_ms=0)
            ok15 = (r["first"] == 401 and r["algorithm"].lower() == "akav1-md5" and r["second"] == 200
                    and "transport=tls" in r["service_route"].lower())
            chk("V15 AKA 등록 (401 AKAv1-MD5 → 200, Service-Route ;transport=tls)", ok15,
                f"{r['first']}/{r['algorithm']}/{r['second']} route={r['service_route'] or '-'}", "401/AKAv1-MD5/200")
            sqn_after = r["sqn"]
            c = sess.unregister_aka(user, domain, auth_user, _K, _OPC, sqn_ms=sqn_after)
            chk("V15b 해제", c == 200, str(c), "200")
        finally:
            sess.close()

        # V16 — 틀린 K → MAC 실패 → 403
        sess = sip_probe.AkaTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
        try:
            r = sess.register_aka(user, domain, auth_user, _BAD_K, _OPC, sqn_ms=0)
            chk("V16 틀린 K (AUTN MAC 실패 보고)", r["first"] == 401 and r["second"] == 403,
                f"{r['first']}/{r['second']}", "401/403")
        finally:
            sess.close()

        # V17 — 단말 SQN_MS 가 앞서 있음 → auts → 재동기 401 → 200
        sess = sip_probe.AkaTlsSession(ctx.sim_ip, tls_port, ctx.sim_ip)
        try:
            r = sess.register_aka(user, domain, auth_user, _K, _OPC, sqn_ms=5000)
            row = _read_row(db_cfg, user)
            ok17 = r["first"] == 401 and r["second"] == 401 and r["resync"] and r["third"] == 200 and r["sqn"] == 5001
            chk("V17 SQN 재동기 (auts → 새 401 → 200, SQN=5001)", ok17,
                f"{r['first']}/{r['second']}/{r['third']} sqn={r['sqn']} db={row[3] if row else '?'}", "401/401/200")
            sess.unregister_aka(user, domain, auth_user, _K, _OPC, sqn_ms=r["sqn"])
        finally:
            sess.close()
    except Exception as e:
        checks.append(False)
        lines.append(f"- [FAIL] 예외: {type(e).__name__}: {e}")
    finally:
        _write_row(db_cfg, user, tuple(orig))
        notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=ctx.sim_ip)
        time.sleep(1.0)

    ok = all(checks) and bool(checks)
    for ln in lines:
        ctx.w(ln)
    ctx.w()
    return ItemResult(id=rid, name=rname, status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail="\n".join(lines), stage=3)
