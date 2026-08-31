"""단말용 MCPTT 서비스 공개 URL 의 단일 정본 단위시험.

McpttServer.PublicUrl 이 있으면 그 값, 없으면 요청 Host 유도 — ue-init-config 의
XCAP-root-URI · openid-configuration · /provisioning/me 의 csc · CSP 에 주는 xcap-root
가 모두 같은 값에서 파생되는지 본다 (CSP 에는 이 주소 설정이 없다).
"""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from httpsrv.handler import HandlerArgs                                   # noqa: E402
from services import mcptt                                               # noqa: E402
from services.auc import auc                                             # noqa: E402


def _args(host=None, method="GET", path="/x"):
    headers = {} if host is None else {"host": host}
    return HandlerArgs(method=method, full_path=path, client_ip="127.0.0.1", client_port=1, headers=headers)


class TestPublicUrl(unittest.TestCase):
    def setUp(self):
        self._saved = (mcptt._MCPTT_PUBLIC_URL, mcptt._MCPTT_PORT)
        mcptt._MCPTT_PUBLIC_URL = ''
        mcptt._MCPTT_PORT = 4430

    def tearDown(self):
        mcptt._MCPTT_PUBLIC_URL, mcptt._MCPTT_PORT = self._saved

    # ── 요청 Host 유도 (PublicUrl 미설정 — 올인원) ──
    def test_host_reflection(self):
        a = _args("10.0.0.5:4430")
        self.assertEqual(mcptt.public_base_url(a), "https://10.0.0.5:4430")
        self.assertEqual(mcptt.public_host_port(a), ("10.0.0.5", 4430))

    def test_host_absent_falls_back_to_domain(self):
        self.assertEqual(mcptt.public_base_url(_args()), f"https://{mcptt.IDMS_DOMAIN}:4430")

    def test_host_without_port(self):
        self.assertEqual(mcptt.public_host_port(_args("ptt.example.com")), ("ptt.example.com", 4430))

    # ── PublicUrl 설정 (VIP·리버스 프록시) ──
    def test_public_url_wins_over_host(self):
        mcptt._MCPTT_PUBLIC_URL = "https://ptt.example.com:8443"
        a = _args("10.0.0.5:4430")
        self.assertEqual(mcptt.public_base_url(a), "https://ptt.example.com:8443")
        self.assertEqual(mcptt.public_host_port(a), ("ptt.example.com", 8443))
        self.assertEqual(mcptt.public_xcap_root(a), "https://ptt.example.com:8443/")

    def test_public_url_without_port_uses_configured_port(self):
        mcptt._MCPTT_PUBLIC_URL = "https://ptt.example.com"
        self.assertEqual(mcptt.public_host_port(_args("10.0.0.5:4430")), ("ptt.example.com", 4430))

    # ── xcap-root: CSP 는 admin(4421) 로 오므로 그 포트를 그대로 쓰면 안 된다 ──
    def test_xcap_root_replaces_admin_port_with_mcptt_port(self):
        self.assertEqual(mcptt.public_xcap_root(_args("10.0.0.5:4421")), "https://10.0.0.5:4430/")

    def test_xcap_root_always_ends_with_slash(self):
        mcptt._MCPTT_PUBLIC_URL = "https://ptt.example.com:8443"
        self.assertTrue(mcptt.public_xcap_root(_args("10.0.0.5:4421")).endswith("/"))

    # ── 단말이 듣는 두 주소의 동일성 (이 변경의 핵심 불변식) ──
    def test_ue_init_config_and_xcap_root_agree(self):
        for pub in ('', 'https://ptt.example.com:8443'):
            mcptt._MCPTT_PUBLIC_URL = pub
            ue_base = mcptt.public_base_url(_args("10.0.0.5:4430"))       # 단말이 4430 으로 옴
            csp_root = mcptt.public_xcap_root(_args("10.0.0.5:4421"))     # CSP 는 4421 로 옴
            self.assertEqual(ue_base + "/", csp_root, f"PublicUrl={pub!r}")


class TestInternalEndpointApi(unittest.TestCase):
    def setUp(self):
        self._saved = (mcptt._MCPTT_PUBLIC_URL, mcptt._MCPTT_PORT)
        mcptt._MCPTT_PORT = 4430
        mcptt._MCPTT_PUBLIC_URL = ''
        auc.init({"AuC": {"Kek": "00112233445566778899aabbccddeeff"}, "InternalApi": {"Token": "tok"}})

    def tearDown(self):
        mcptt._MCPTT_PUBLIC_URL, mcptt._MCPTT_PORT = self._saved

    def _call(self, headers, method="GET"):
        from handlers.internal_api import handle_mcptt_endpoint
        ha = HandlerArgs(method=method, full_path="/internal/mcptt/endpoint", client_ip="127.0.0.1",
                         client_port=1, headers=headers)
        return asyncio.run(handle_mcptt_endpoint(ha, {"config": {}}))

    def test_token_gate(self):
        self.assertEqual(self._call({"authorization": "Bearer wrong"}).status, 401)
        self.assertEqual(self._call({}).status, 401)
        self.assertEqual(self._call({"Authorization": "Bearer tok"}, method="POST").status, 405)

    def test_returns_derived_root(self):
        r = self._call({"Authorization": "Bearer tok", "host": "10.0.0.5:4421"})
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["xcap_root"], "https://10.0.0.5:4430/")
        self.assertEqual(r.body["mcptt_port"], 4430)
        self.assertFalse(r.body["public_url_configured"])

    def test_returns_configured_public_url(self):
        mcptt._MCPTT_PUBLIC_URL = "https://ptt.example.com:8443"
        r = self._call({"Authorization": "Bearer tok", "host": "10.0.0.5:4421"})
        self.assertEqual(r.body["xcap_root"], "https://ptt.example.com:8443/")
        self.assertTrue(r.body["public_url_configured"])

    def test_token_unset_is_503(self):
        auc.init({"AuC": {"Kek": "00112233445566778899aabbccddeeff"}, "InternalApi": {"Token": ""}})
        self.assertEqual(self._call({"Authorization": "Bearer tok"}).status, 503)


if __name__ == '__main__':
    unittest.main()
