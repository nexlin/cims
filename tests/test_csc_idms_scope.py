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
        granted, dropped = m.grant_scope("openid 3gpp:mc:ptt_service 3gpp:mc:video_service cims:provisioning bogus")
        self.assertEqual(granted.split(), ["openid", "3gpp:mc:ptt_service", "cims:provisioning"])
        self.assertEqual(dropped, ["3gpp:mc:video_service", "bogus"])

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
        r = m.unauthorized(_args(auth=None))
        self.assertEqual(r.status, 401)
        self.assertEqual(r.headers["WWW-Authenticate"], f'Bearer realm="{m.IDMS_DOMAIN}"')
        r = m.unauthorized(_args(auth="Bearer bad"))
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


if __name__ == "__main__":
    unittest.main()
