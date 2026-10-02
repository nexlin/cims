"""csc/src/services/mcptt.py — IdMS scope·claim·issuer 규격 정합 단위 시험 (오프라인, DB 없음).

TS 33.180 Annex B: scope 는 요청 ∩ 카탈로그(B.4.2.2 `3gpp:mc:*`), access token 은 공백 구분 `scope` 문자열 +
`client_id` + `mcptt_id`/`mcdata_id`(B.2.2), 리소스 서버는 자기 scope 를 검사한다(B.10 — IdMs.ScopeEnforcement
off/log/enforce). 구 scope `3gpp:mcptt:ptt_server` 는 전환기 별칭으로 MC 서비스 8종 전체로 확장된다.
issuer 는 IdMs.Issuer > McpttServer.PublicUrl > idms.<PTT 도메인> 순으로 유도된다(mcx_identity_scope.md).

  python3 -m unittest tests.test_csc_idms_scope
"""
from __future__ import annotations

import os
import sys
import types
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))
# 의존성은 시스템이 아니라 **vendor** 에 있다(DEV_SERVER_SETUP.md — `pip install --target=csc/vendor`).
# unittest 는 시험 모듈을 **먼저 전부 import** 하므로, 최상위 import 가 csc 패키지를 끌어오는 시험은
# 그 시점에 경로가 서 있어야 한다 — 함수 안에서 잡으면 늦다(loguru ModuleNotFoundError).
for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
    if os.path.isdir(_v) and _v not in sys.path:
        sys.path.append(_v)
        break

import jwt  # noqa: E402
import services.mcptt as m  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

MC8 = list(m.SCOPE_MC_SERVICES)
LEGACY = m.SCOPE_LEGACY_MCPTT


def _args(method="GET", auth="Bearer x"):
    h = {"authorization": auth} if auth else {}
    return HandlerArgs(method, "/x", "127.0.0.1", 0, headers=h)


class GrantScopeTest(unittest.TestCase):
    def test_new_names_pass_unknown_dropped(self):
        granted, dropped = m.grant_scope("openid 3gpp:mc:ptt_service cims:provisioning bogus")
        self.assertEqual(granted.split(), ["openid", "3gpp:mc:ptt_service", "cims:provisioning"])
        self.assertEqual(dropped, ["bogus"])

    def test_video_scopes_need_mcvideo_profile(self):
        """MCVideo 4종은 카탈로그 안이지만 사용자 단위 인가 — mcvideo_user_profile 행이 있을 때만 준다(mcvideo.md §5.1)."""
        import services.mcvideo as mv
        keep = (dict(m.USERS), dict(mv.MCVIDEO_PROFILES))
        try:
            m.USERS["tel:+82510009001"] = {"msisdn": "+82510009001", "name": "a"}
            m.USERS["tel:+82510009002"] = {"msisdn": "+82510009002", "name": "b"}
            mv.MCVIDEO_PROFILES.clear()
            mv.MCVIDEO_PROFILES["+82510009001"] = {"max_video_streams": 1, "max_calls_n6": 1}
            req = "openid 3gpp:mc:ptt_service 3gpp:mc:video_service 3gpp:mc:video_config_management_service"
            g_ok, d_ok = m.grant_scope(req, mcptt_id="tel:+82510009001")
            self.assertEqual(g_ok.split(), req.split())
            self.assertEqual(d_ok, [])
            g_no, d_no = m.grant_scope(req, mcptt_id="sip:+82510009002@ptt.example")
            self.assertEqual(g_no.split(), ["openid", "3gpp:mc:ptt_service"])
            self.assertEqual(d_no, ["3gpp:mc:video_service", "3gpp:mc:video_config_management_service"])
            # 신원 없이 부르면 카탈로그만 본다(발급 경로는 늘 신원을 넘긴다)
            self.assertIn("3gpp:mc:video_service", m.grant_scope(req)[0].split())
        finally:
            m.USERS.clear(); m.USERS.update(keep[0])
            mv.MCVIDEO_PROFILES.clear(); mv.MCVIDEO_PROFILES.update(keep[1])

    def test_legacy_alias_expands_to_all_mc_and_keeps_literal(self):
        granted, dropped = m.grant_scope(f"openid {LEGACY}")
        toks = granted.split()
        self.assertEqual(toks[0], "openid")
        self.assertEqual(toks[1], LEGACY, "구 문자열 병기(요청 scope 문자열 대조 단말 호환)")
        self.assertEqual(toks[2:], MC8)
        self.assertEqual(dropped, [])

    def test_empty_and_list_input(self):
        self.assertEqual(m.grant_scope(""), ("", []))
        self.assertEqual(m.grant_scope(["3gpp:mc:data_service"])[0], "3gpp:mc:data_service")

    def test_expand_and_token_scopes_accept_legacy_array_token(self):
        # 이행 전 발급 토큰: scope 가 JSON 배열 + 구 문자열
        legacy_token = {"scope": [LEGACY]}
        have = m.token_scopes(legacy_token)
        self.assertTrue(set(MC8) <= have)
        self.assertIn(LEGACY, have)
        # 문자열형 신 토큰
        self.assertEqual(m.token_scopes({"scope": "openid 3gpp:mc:ptt_service"}), {"openid", "3gpp:mc:ptt_service"})
        self.assertEqual(m.token_scopes({}), set())


class CreateTokensTest(unittest.TestCase):
    def setUp(self):
        self._storage = m.storage
        self.saved = {}
        m.storage = types.SimpleNamespace(save_refresh_token=lambda tok, data: self.saved.update({tok: data}))
        self._secret = m.SECRET_KEY
        m.SECRET_KEY = "unit-test-secret"

    def tearDown(self):
        m.storage = self._storage
        m.SECRET_KEY = self._secret

    def test_access_and_id_token_claims(self):
        scope, _ = m.grant_scope(f"openid cims:provisioning {LEGACY}")
        id_tok, acc, ref = m.create_tokens("test003", scope, "MCPTT_UE", nonce="n1", mcptt_id="tel:+82500000003")
        a = jwt.decode(acc, "unit-test-secret", algorithms=["HS256"], audience="mcptt_client")
        self.assertIsInstance(a["scope"], str, "B.2.2.2: scope 는 공백 구분 문자열")
        self.assertEqual(a["scope"], scope)
        self.assertEqual(a["client_id"], "MCPTT_UE")
        self.assertEqual(a["mcptt_id"], "tel:+82500000003")
        self.assertEqual(a["mcdata_id"], "tel:+82500000003", "단일 MC service ID — mcdata_id = mcptt_id")
        self.assertEqual(a["sub"], "test003")
        i = jwt.decode(id_tok, "unit-test-secret", algorithms=["HS256"], audience="MCPTT_UE")
        self.assertEqual(i["mcdata_id"], "tel:+82500000003")
        self.assertEqual(i["nonce"], "n1")
        self.assertEqual(self.saved[ref]["scope"], scope)

    def test_refresh_scope_kept_broad(self):
        _, _, ref = m.create_tokens("u", "3gpp:mc:ptt_service", "MCPTT_UE", refresh_scope="openid " + LEGACY)
        self.assertEqual(self.saved[ref]["scope"], "openid " + LEGACY)


class RequireScopeTest(unittest.TestCase):
    def setUp(self):
        self._mode = m.SCOPE_ENFORCEMENT

    def tearDown(self):
        m.SCOPE_ENFORCEMENT = self._mode

    def test_enforce_denies_with_rfc6750_header(self):
        m.SCOPE_ENFORCEMENT = "enforce"
        tok = {"scope": "openid cims:provisioning", "mcptt_id": "tel:+1", "client_id": "MCPTT_UE"}
        r = m.require_scope(_args(), tok, "GMS", m.SCOPE_PTT_GMS, m.SCOPE_DATA_GMS)
        self.assertIsNotNone(r)
        self.assertEqual(r.status, 403)
        self.assertEqual(r.body["error"], "insufficient_scope")
        www = r.headers["WWW-Authenticate"]
        self.assertTrue(www.startswith("Bearer "))
        self.assertIn('error="insufficient_scope"', www)
        self.assertIn(m.SCOPE_PTT_GMS, www)

    def test_enforce_passes_any_of(self):
        m.SCOPE_ENFORCEMENT = "enforce"
        self.assertIsNone(m.require_scope(_args(), {"scope": "3gpp:mc:data_group_management_service"}, "GMS",
                                          m.SCOPE_PTT_GMS, m.SCOPE_DATA_GMS))
        # 구 토큰(배열·별칭) 도 enforce 통과
        self.assertIsNone(m.require_scope(_args(), {"scope": [LEGACY]}, "KMS", m.SCOPE_PTT_KMS, m.SCOPE_DATA_KMS))
        self.assertIsNone(m.require_scope(_args(), {"scope": [LEGACY]}, "MCDATA-FD", m.SCOPE_DATA_SERVICE))

    def test_log_mode_passes_and_off_skips(self):
        m.SCOPE_ENFORCEMENT = "log"
        self.assertIsNone(m.require_scope(_args(), {"scope": "openid"}, "CMS", m.SCOPE_PTT_CMS))
        m.SCOPE_ENFORCEMENT = "off"
        self.assertIsNone(m.require_scope(_args(), {"scope": ""}, "CMS", m.SCOPE_PTT_CMS))

    def test_empty_scope_denied_under_enforce(self):
        m.SCOPE_ENFORCEMENT = "enforce"
        r = m.require_scope(_args(), {"scope": ""}, "CMS", m.SCOPE_PTT_CMS)
        self.assertEqual(r.status, 403)

    def test_unauthorized_header(self):
        # TS 24.482 A.2.3 — Bearer 토큰이 없으면 403(1)), 있었는데 검증 실패면 401 invalid_token(2)a) → RFC 6750 §3.1)
        r = m.unauthorized(_args(auth=None))
        self.assertEqual(r.status, 403)
        self.assertEqual(m.unauthorized(_args(auth="Basic abc")).status, 403, "Bearer scheme 이 아니면 토큰 없음과 같다")
        self.assertEqual(m.unauthorized(_args(auth="Bearer ")).status, 403)
        r = m.unauthorized(_args(auth="Bearer bad"))
        self.assertEqual(r.status, 401)
        self.assertIn('error="invalid_token"', r.headers["WWW-Authenticate"])


class IssuerDerivationTest(unittest.TestCase):
    D = "ptt.cims.example.kr"

    def test_derived_from_ptt_domain(self):
        self.assertEqual(m.resolve_idms_identity({}, self.D, ""),
                         (f"idms.{self.D}", self.D, f"kms.{self.D}"))

    def test_public_url_wins_over_fqdn(self):
        iss, dom, kms = m.resolve_idms_identity({}, self.D, "https://121.161.164.45:4430")
        self.assertEqual(iss, "https://121.161.164.45:4430")
        self.assertEqual((dom, kms), (self.D, f"kms.{self.D}"))

    def test_explicit_config_wins(self):
        iss, dom, kms = m.resolve_idms_identity({"Issuer": "idms.x", "Domain": "x", "KmsUri": "kms.x"},
                                                self.D, "https://pub")
        self.assertEqual((iss, dom, kms), ("idms.x", "x", "kms.x"))

    def test_code_default_when_nothing(self):
        iss, dom, kms = m.resolve_idms_identity({}, "", "")
        self.assertEqual(dom, m._IDMS_DOMAIN_DEFAULT)
        self.assertEqual(iss, f"idms.{m._IDMS_DOMAIN_DEFAULT}")


class DiscoveryTest(unittest.TestCase):
    def test_scopes_and_claims_supported(self):
        import asyncio
        r = asyncio.run(
            m.handle_openid_config(HandlerArgs("GET", "/.well-known/openid-configuration", "127.0.0.1", 0,
                                               headers={"host": "csc.example:4430"}), {}))
        doc = r.body
        self.assertEqual(doc["issuer"], m.IDMS_ISSUER)
        for s in MC8 + [LEGACY, "openid", "cims:provisioning"]:
            self.assertIn(s, doc["scopes_supported"])
        for c in ("client_id", "mcptt_id", "mcdata_id", "scope"):
            self.assertIn(c, doc["claims_supported"])


class _MemStorage:
    """IdmsStorage 인메모리 대체 — 발급 경로(authreq→tokenreq→refresh)를 프로세스 안에서 돈다."""
    def __init__(self):
        self.codes, self.tokens = {}, {}
    def save_auth_code(self, code, data): self.codes[code] = dict(data); return True
    def get_auth_code(self, code): return self.codes.get(code)
    def delete_auth_code(self, code): return self.codes.pop(code, None) is not None
    def save_refresh_token(self, tok, data): self.tokens[tok] = dict(data); return True
    def get_refresh_token(self, tok): return self.tokens.get(tok)
    def revoke_refresh_token(self, tok, rotated_to=None):
        if tok in self.tokens:
            self.tokens[tok]["revoked"] = True; self.tokens[tok]["rotated_to"] = rotated_to
        return True


class IssuanceFlowTest(unittest.TestCase):
    """간이형 authreq(GET+자격) → tokenreq(authorization_code) → tokenreq(refresh_token) 를 핸들러 직접 호출로."""
    LOGIN, PW, PTT = "unit-login", "pw", "tel:+82500009999"

    def setUp(self):
        import asyncio, base64, hashlib
        self._keep = (m.storage, m.SECRET_KEY, dict(m.LOGIN_ACCOUNTS), m.SCOPE_ENFORCEMENT)
        m.storage = _MemStorage(); m.SECRET_KEY = "unit-flow-secret"
        m.LOGIN_ACCOUNTS[self.LOGIN] = {"user_id": 1, "mcptt_id": self.PTT, "password": self.PW, "name": "u"}
        self.run_ = asyncio.run
        self.verifier = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
        self.challenge = base64.urlsafe_b64encode(hashlib.sha256(self.verifier.encode()).digest()).rstrip(b"=").decode()

    def tearDown(self):
        m.storage, m.SECRET_KEY, accts, m.SCOPE_ENFORCEMENT = self._keep
        m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(accts)

    def _login(self, scope):
        q = {"user_name": self.LOGIN, "user_password": self.PW, "client_id": "MCPTT_UE",
             "redirect_uri": "http://localhost/cb", "code_challenge": self.challenge,
             "code_challenge_method": "S256", "scope": scope}
        r = self.run_(m.handle_auth_req(HandlerArgs("GET", "/idms/authreq", "127.0.0.1", 0, query_params=q), {}))
        self.assertEqual(r.status, 200, r.body)
        code = r.body["code"]
        r = self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body={
            "grant_type": "authorization_code", "code": code, "code_verifier": self.verifier,
            "client_id": "MCPTT_UE", "redirect_uri": "http://localhost/cb"}), {}))
        self.assertEqual(r.status, 200, r.body)
        return r.body

    def _refresh(self, tok, scope=None):
        body = {"grant_type": "refresh_token", "refresh_token": tok, "client_id": "MCPTT_UE"}
        if scope is not None:
            body["scope"] = scope
        r = self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body=body), {}))
        self.assertEqual(r.status, 200, r.body)
        return r.body

    def test_legacy_request_expands_and_response_carries_scope(self):
        t = self._login("openid " + LEGACY)
        self.assertEqual(t["scope"].split(), ["openid", LEGACY] + MC8)
        self.assertEqual(t["expires_in"], m.ACCESS_TOKEN_TTL)
        pl = jwt.decode(t["access_token"], "unit-flow-secret", algorithms=["HS256"], audience="mcptt_client")
        self.assertEqual(pl["scope"], t["scope"]); self.assertEqual(pl["client_id"], "MCPTT_UE")
        self.assertEqual(pl["mcdata_id"], self.PTT)

    def test_unknown_scope_dropped(self):
        t = self._login("openid 3gpp:mc:ptt_service 3gpp:mc:video_service")
        self.assertEqual(t["scope"], "openid 3gpp:mc:ptt_service")

    def test_refresh_narrowing_over_alias_and_broad_kept(self):
        t = self._login("openid cims:provisioning " + LEGACY)
        r1 = self._refresh(t["refresh_token"], "3gpp:mc:data_service")
        self.assertEqual(r1["scope"], "3gpp:mc:data_service")
        r2 = self._refresh(r1["refresh_token"], "cims:provisioning")
        self.assertEqual(r2["scope"], "cims:provisioning", "회전된 refresh 도 원 grant(broad) 를 보존")
        r3 = self._refresh(r2["refresh_token"])
        self.assertEqual(r3["scope"].split(), ["openid", "cims:provisioning", LEGACY] + MC8)
        # 이행 전 저장된 refresh(구 문자열만) 로 신 이름 요청 → 별칭 확장 위에서 교집합
        m.storage.tokens["old-ref"] = {"user_id": self.LOGIN, "mcptt_id": self.PTT, "client_id": "MCPTT_UE",
                                       "scope": LEGACY, "issued_at": 0, "expires_at": 2**31, "revoked": False}
        r4 = self._refresh("old-ref", "3gpp:mc:ptt_group_management_service")
        self.assertEqual(r4["scope"], "3gpp:mc:ptt_group_management_service")

    def test_gms_gate_with_issued_tokens(self):
        m.SCOPE_ENFORCEMENT = "enforce"
        weak = self._login("openid 3gpp:mc:ptt_service")["access_token"]
        r = m.require_scope(_args(), m.validate_access_token(weak), "GMS", m.SCOPE_PTT_GMS, m.SCOPE_DATA_GMS)
        self.assertEqual(r.status, 403)
        strong = self._login("openid " + LEGACY)["access_token"]
        self.assertIsNone(m.require_scope(_args(), m.validate_access_token(strong), "GMS", m.SCOPE_PTT_GMS, m.SCOPE_DATA_GMS))


class MemberOnlyAndClientRegistrationTest(IssuanceFlowTest):
    """C06 — IDM-1(MC scope·신원은 PTT 가입자에게만) · IDM-2·3·4(필수 파라미터·클라이언트 등록, IdMs.ClientEnforcement) ·
    IDM-7(refresh 때 계정 재확인) · IDM-8(no-store) · CMS-10(Bearer 없음 = 403)."""
    PHONE, PHONE_LINE = "unit-phone", "tel:+821300009999"

    def setUp(self):
        super().setUp()
        self._client = (m.CLIENT_ENFORCEMENT, dict(m.IDMS_CLIENTS))
        m.LOGIN_ACCOUNTS[self.LOGIN]["line_id"] = self.PTT
        # 전화 전용 계정 — PTT 가입 없음(mcptt_id None), 회선은 VoLTE
        m.LOGIN_ACCOUNTS[self.PHONE] = {"user_id": 2, "mcptt_id": None, "line_id": self.PHONE_LINE, "password": "pw2", "name": "p"}

    def tearDown(self):
        m.CLIENT_ENFORCEMENT, clients = self._client
        m.IDMS_CLIENTS.clear(); m.IDMS_CLIENTS.update(clients)
        super().tearDown()

    def _auth(self, login, pw, **over):
        q = {"user_name": login, "user_password": pw, "client_id": "MCPTT_UE", "redirect_uri": "http://localhost/cb",
             "code_challenge": self.challenge, "code_challenge_method": "S256", "scope": "openid cims:provisioning " + LEGACY,
             "response_type": "code", "state": "s1", "acr_values": "3gpp:acr:password"}
        q.update(over)
        q = {k: v for k, v in q.items() if v is not None}
        return self.run_(m.handle_auth_req(HandlerArgs("GET", "/idms/authreq", "127.0.0.1", 0, query_params=q), {}))

    def _token(self, code, **over):
        body = {"grant_type": "authorization_code", "code": code, "code_verifier": self.verifier,
                "client_id": "MCPTT_UE", "redirect_uri": "http://localhost/cb"}
        body.update(over)
        body = {k: v for k, v in body.items() if v is not None}
        return self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body=body), {}))

    # ── IDM-1 (TS 24.482 §4.1 · TS 33.180 B.4.2.2) ──
    def test_phone_only_account_gets_no_mc_scope_or_identity(self):
        r = self._auth(self.PHONE, "pw2")
        self.assertEqual(r.status, 200, r.body)
        t = self._token(r.body["code"])
        self.assertEqual(t.status, 200, t.body)
        self.assertEqual(t.body["scope"], "openid cims:provisioning", "MC scope(구 별칭 포함)는 PTT 가입자에게만")
        for tok, aud in ((t.body["access_token"], "mcptt_client"), (t.body["id_token"], "MCPTT_UE")):
            pl = jwt.decode(tok, "unit-flow-secret", algorithms=["HS256"], audience=aud)
            for c in ("mcptt_id", "mcdata_id", "mcvideo_id"):
                self.assertNotIn(c, pl)
            self.assertEqual(pl["sub"], self.PHONE)
        acc = m.validate_access_token(t.body["access_token"])
        self.assertEqual(m.token_line_id(acc), self.PHONE_LINE, "프로비저닝은 로그인 계정의 회선으로 사람을 찾는다")
        # KMS 는 MC 신원 없는 토큰에 키를 내지 않는다
        keep = m.extract_token
        try:
            m.extract_token = lambda hdr: acc
            r = self.run_(m.handle_kms_keyprov(_args(), {}))
            self.assertEqual(r.status, 403)
        finally:
            m.extract_token = keep
        # refresh 로도 MC scope 를 얻지 못한다(옛 refresh token 이 MC scope 를 들고 있어도)
        m.storage.tokens["old-phone"] = {"user_id": self.PHONE, "mcptt_id": self.PHONE_LINE, "client_id": "MCPTT_UE",
                                         "scope": "openid cims:provisioning " + LEGACY, "issued_at": 0,
                                         "expires_at": 2**31, "revoked": False}
        r = self._refresh("old-phone", "3gpp:mc:ptt_service")
        self.assertEqual(r["scope"], "openid cims:provisioning")
        self.assertNotIn("mcptt_id", jwt.decode(r["access_token"], "unit-flow-secret", algorithms=["HS256"], audience="mcptt_client"))

    def test_ptt_account_unchanged_and_line_id(self):
        t = self._token(self._auth(self.LOGIN, self.PW).body["code"]).body
        self.assertEqual(t["scope"].split(), ["openid", "cims:provisioning", LEGACY] + MC8)
        self.assertEqual(m.token_line_id(m.validate_access_token(t["access_token"])), self.PTT)

    def test_load_login_accounts_mcptt_id_only_from_ptt_line(self):
        class Cur:
            def execute(self, sql, params=None): pass
            def fetchall(self):
                return [{"uid": 1, "login_id": "a", "passwd": "x", "name": "A", "ptt": "+8250001", "volte": "+8213001", "voip": None},
                        {"uid": 2, "login_id": "b", "passwd": "y", "name": "B", "ptt": None, "volte": "+8213002", "voip": None},
                        {"uid": 3, "login_id": "c", "passwd": "z", "name": "C", "ptt": None, "volte": None, "voip": None}]
        keep_has, keep_acc = m._subs.has_table, dict(m.LOGIN_ACCOUNTS)
        try:
            m._subs.has_table = lambda cur, kind: False
            m._load_login_accounts(Cur())
            self.assertEqual((m.LOGIN_ACCOUNTS["a"]["mcptt_id"], m.LOGIN_ACCOUNTS["a"]["line_id"]), ("tel:+8250001", "tel:+8250001"))
            self.assertEqual((m.LOGIN_ACCOUNTS["b"]["mcptt_id"], m.LOGIN_ACCOUNTS["b"]["line_id"]), (None, "tel:+8213002"))
            self.assertEqual((m.LOGIN_ACCOUNTS["c"]["mcptt_id"], m.LOGIN_ACCOUNTS["c"]["line_id"]), (None, None))
        finally:
            m._subs.has_table = keep_has
            m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(keep_acc)

    # ── IDM-2·3 (TS 33.180 표 B.4.2.2-1 · B.3) ──
    def test_auth_request_required_params_and_registration(self):
        m.IDMS_CLIENTS.clear(); m.IDMS_CLIENTS["MCPTT_UE"] = {"http://localhost/cb"}
        m.CLIENT_ENFORCEMENT = "enforce"
        self.assertEqual(self._auth(self.LOGIN, self.PW).status, 200)
        for missing in ("response_type", "state", "acr_values", "client_id", "scope"):
            r = self._auth(self.LOGIN, self.PW, **{missing: None})
            self.assertEqual(r.status, 400, missing)
            self.assertIn(missing, r.body["error_description"])
        self.assertEqual(self._auth(self.LOGIN, self.PW, scope="3gpp:mc:ptt_service").status, 400, "scope 에 openid 필수")
        self.assertEqual(self._auth(self.LOGIN, self.PW, acr_values="urn:other").status, 400)
        r = self._auth(self.LOGIN, self.PW, client_id="rogue")
        self.assertIn("not registered", r.body["error_description"])
        r = self._auth(self.LOGIN, self.PW, redirect_uri="http://evil/cb")
        self.assertEqual(r.status, 400)
        self.assertIn("redirect_uri", r.body["error_description"])
        # log(기본) — 로그만 남기고 통과 · off — 검사 없음
        for mode in ("log", "off"):
            m.CLIENT_ENFORCEMENT = mode
            self.assertEqual(self._auth(self.LOGIN, self.PW, client_id="rogue", state=None, acr_values=None).status, 200, mode)

    def test_parse_idms_clients(self):
        got = m.parse_idms_clients([{"ClientId": "app1", "RedirectUris": "cims://cb, http://localhost/cb"},
                                    {"ClientId": "app2", "RedirectUris": ["x://y"]}, {"ClientId": ""}, "junk"])
        self.assertEqual(got, {"app1": {"cims://cb", "http://localhost/cb"}, "app2": {"x://y"}})
        self.assertEqual(m.parse_idms_clients(None), {})

    # ── IDM-4 (표 B.4.2.4-1) ──
    def test_token_request_requires_client_and_redirect(self):
        m.IDMS_CLIENTS.clear(); m.IDMS_CLIENTS["MCPTT_UE"] = {"http://localhost/cb"}
        m.CLIENT_ENFORCEMENT = "enforce"
        code = self._auth(self.LOGIN, self.PW).body["code"]
        r = self._token(code, redirect_uri=None)
        self.assertEqual((r.status, r.body["error"]), (400, "invalid_grant"))
        self.assertIn("redirect_uri is required", r.body["error_description"])
        self.assertEqual(self._token(code, redirect_uri="http://other/cb").status, 400, "인증 요청과 다른 redirect_uri")
        ok = self._token(code)
        self.assertEqual(ok.status, 200, ok.body)
        m.CLIENT_ENFORCEMENT = "log"
        code = self._auth(self.LOGIN, self.PW).body["code"]
        self.assertEqual(self._token(code, redirect_uri=None).status, 200, "log 모드는 통과")

    # ── IDM-8 (B.4.2.5·B.5.3 · RFC 6749 §5.1) ──
    def test_token_responses_are_no_store(self):
        t = self._token(self._auth(self.LOGIN, self.PW).body["code"])
        self.assertEqual((t.headers["Cache-Control"], t.headers["Pragma"]), ("no-store", "no-cache"))
        r = self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body={
            "grant_type": "refresh_token", "refresh_token": t.body["refresh_token"]}), {}))
        self.assertEqual(r.status, 200, "refresh 요청의 client_id 는 필수가 아니다(표 B.5.2-1)")
        self.assertEqual(r.headers["Cache-Control"], "no-store")
        bad = self._token("no-such-code")
        self.assertEqual((bad.status, bad.headers["Cache-Control"]), (400, "no-store"))

    # ── IDM-7 (B.5.3 RECOMMENDED) ──
    def test_refresh_revalidates_account(self):
        t = self._token(self._auth(self.LOGIN, self.PW).body["code"]).body
        ref = t["refresh_token"]
        self.assertEqual(m.storage.tokens[ref]["cred"], m.account_cred(self.LOGIN))
        m.LOGIN_ACCOUNTS[self.LOGIN]["password"] = "changed"                 # 비밀번호 변경
        r = self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body={
            "grant_type": "refresh_token", "refresh_token": ref, "client_id": "MCPTT_UE"}), {}))
        self.assertEqual((r.status, r.body["error"]), (400, "invalid_grant"))
        self.assertTrue(m.storage.tokens[ref]["revoked"], "회수한다")
        m.LOGIN_ACCOUNTS[self.LOGIN]["password"] = self.PW
        ref2 = self._token(self._auth(self.LOGIN, self.PW).body["code"]).body["refresh_token"]
        del m.LOGIN_ACCOUNTS[self.LOGIN]                                    # 계정 삭제
        r = self.run_(m.handle_token_req(HandlerArgs("POST", "/idms/tokenreq", "127.0.0.1", 0, body={
            "grant_type": "refresh_token", "refresh_token": ref2, "client_id": "MCPTT_UE"}), {}))
        self.assertEqual(r.status, 400)
        self.assertTrue(m.storage.tokens[ref2]["revoked"])


if __name__ == "__main__":
    unittest.main()
