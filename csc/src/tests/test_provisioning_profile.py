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


def _profile(svc_cfg: dict, kind: str = "ptt") -> dict:
    saved = mcptt.PROVISIONING
    mcptt.PROVISIONING = {"Services": {kind: svc_cfg}}
    try:
        return mcptt._provision_service(kind, "+82500000001", "450081050000001", "", "10.0.0.5")
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


if __name__ == '__main__':
    unittest.main()
