"""/provisioning/me 서비스 프로파일 생성(_provision_service) 단위시험.

mediaSecurity(미디어 SRTP 정책, media_security.md §7.2) — 설정
Provisioning.Services.<kind>.media_srtp 를 그대로 내리되 이상값은 off 로
강등하는지(조용한 상향 금지) 본다.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from services import mcptt                                               # noqa: E402


def _profile(svc_cfg: dict, kind: str = "ptt", **kw) -> dict:
    saved = mcptt.PROVISIONING
    mcptt.PROVISIONING = {"Services": {kind: svc_cfg}}
    try:
        return mcptt._provision_service(kind, "+82500000001", "450081050000001", "", "10.0.0.5", **kw)
    finally:
        mcptt.PROVISIONING = saved


class TestMediaSecurity(unittest.TestCase):
    def test_default_off(self):
        self.assertEqual(_profile({})["sip"]["mediaSecurity"], "off")

    def test_configured_values_pass_through(self):
        for v in ("off", "optional", "required"):
            self.assertEqual(_profile({"media_srtp": v})["sip"]["mediaSecurity"], v)

    def test_case_normalized(self):
        self.assertEqual(_profile({"media_srtp": "Required"})["sip"]["mediaSecurity"], "required")

    def test_invalid_value_falls_back_to_off(self):
        for v in ("on", "true", 1, None, ""):
            self.assertEqual(_profile({"media_srtp": v})["sip"]["mediaSecurity"], "off",
                             f"media_srtp={v!r}")

    def test_both_kinds_carry_field(self):
        for kind in ("volte", "ptt"):
            self.assertIn("mediaSecurity", _profile({"media_srtp": "optional"}, kind)["sip"])


class TestTransportEnforced(unittest.TestCase):
    """transport 목록 축소/enforced — 서버 게이트와 같은 술어
    (sip_access_security.md §2.2·§8.2: requiresTls = sip_transport=TLS ∨ auth_scheme=aka)."""
    TLS_SVC = {"tls_port": 15061}
    AKA = {"k": "0" * 32, "opc": "1" * 32, "amf": "8000"}

    def test_digest_free_choice(self):
        sip = _profile(self.TLS_SVC)["sip"]
        self.assertFalse(sip["enforced"])
        self.assertEqual({t["transport"] for t in sip["transports"]}, {"UDP", "TCP", "TLS"})

    def test_tls_policy_narrows(self):
        sip = _profile(self.TLS_SVC, sip_transport="TLS")["sip"]
        self.assertTrue(sip["enforced"])
        self.assertEqual([t["transport"] for t in sip["transports"]], ["TLS"])

    def test_aka_narrows_like_gate(self):
        # AKA 가입자는 sip_transport 값과 무관하게 보호 채널 강제 — 목록도 TLS 로 좁힌다
        for transport in ("", "UDP"):
            sip = _profile(self.TLS_SVC, sip_transport=transport, aka=self.AKA)["sip"]
            self.assertTrue(sip["enforced"], f"sip_transport={transport!r}")
            self.assertEqual([t["transport"] for t in sip["transports"]], ["TLS"])
            self.assertEqual(sip["default"], "TLS")

    def test_aka_without_tls_port_cannot_narrow(self):
        # 운영 오설정(tls_port=0) — 도달 경로가 없어 좁히지 못한다(단말은 403 을 받는다)
        sip = _profile({}, aka=self.AKA)["sip"]
        self.assertFalse(sip["enforced"])

    def test_aka_with_ipsec_keeps_choices(self):
        # 서비스가 ipsec-3gpp 를 제시하면 AKA 의 유효 채널이 TLS/IPsec 두 갈래 — 좁히지 않는다
        svc = dict(self.TLS_SVC, sec_mechanisms=["tls", "ipsec-3gpp"],
                   ipsec_port_ps=25062, ipsec_port_pc=25063)
        sip = _profile(svc, aka=self.AKA)["sip"]
        self.assertFalse(sip["enforced"])
        self.assertIn("UDP", {t["transport"] for t in sip["transports"]})

    def test_aka_with_ipsec_ports_missing_narrows(self):
        # ipsec-3gpp 제시했으나 포트쌍 미설정 = 목록에서 빠짐 — TLS 단독으로 좁힌다
        svc = dict(self.TLS_SVC, sec_mechanisms=["tls", "ipsec-3gpp"])
        sip = _profile(svc, aka=self.AKA)["sip"]
        self.assertTrue(sip["enforced"])
        self.assertNotIn("ipsec-3gpp", sip["security"])


class TestServiceEntry(unittest.TestCase):
    """service_entry — 가입 행 service_ref(= access_services.name)로 Provisioning.Services 항목을 고른다
    (sip_service_model.md §2-9: 유선 voip·이동 volte 회선이 같은 volte_subscriptions 에 있어 종류 키만으로는 도메인이 어긋난다)."""
    SERVICES = {"Services": {
        "volte": {"name": "volte", "domain": "volte.example"},
        "voip": {"name": "voip", "domain": "voip.example", "tls_port": 15061, "transport": "TLS"},
        "ptt": {"name": "mcptt", "domain": "ptt.example"},
    }}

    def setUp(self):
        self._saved = mcptt.PROVISIONING
        mcptt.PROVISIONING = self.SERVICES

    def tearDown(self):
        mcptt.PROVISIONING = self._saved

    def test_ref_selects_named_entry_and_wire_kind_is_entry_key(self):
        kind, svc = mcptt.service_entry("volte", "voip")
        self.assertEqual((kind, svc["domain"]), ("voip", "voip.example"))
        kind, svc = mcptt.service_entry("ptt", "mcptt")            # 라이브 name(mcptt) ≠ 키(ptt) — 이름 매칭
        self.assertEqual((kind, svc["domain"]), ("ptt", "ptt.example"))

    def test_empty_or_unknown_ref_falls_back_to_kind(self):
        for ref in ("", None, "  ", "no-such"):
            kind, svc = mcptt.service_entry("volte", ref)
            self.assertEqual((kind, svc["domain"]), ("volte", "volte.example"), f"ref={ref!r}")

    def test_ref_never_crosses_kind_boundary(self):
        # PTT 가입 행이 전화 서비스를 가리켜도 ptt 항목 유지(가입 테이블이 다르다) — 반대도 같다
        self.assertEqual(mcptt.service_entry("ptt", "voip")[0], "ptt")
        self.assertEqual(mcptt.service_entry("volte", "mcptt")[0], "volte")

    def test_provisioned_voip_line_carries_voip_policy(self):
        out = mcptt._provision_service("volte", "+821310001001", "45033821310001001", "", "10.0.0.5",
                                       sip_transport="TLS", sip_ha1="0" * 32, service_ref="voip")
        self.assertEqual(out["kind"], "voip")
        self.assertEqual(out["sip"]["domain"], "voip.example")
        self.assertEqual(out["sip"]["transport"], "TLS")
        self.assertTrue(out["sip"]["enforced"])
        self.assertNotIn("mcpttId", out["account"])

    def test_services_missing_entirely(self):
        mcptt.PROVISIONING = {}
        self.assertEqual(mcptt.service_entry("volte", "voip"), ("volte", {}))


if __name__ == '__main__':
    unittest.main()
