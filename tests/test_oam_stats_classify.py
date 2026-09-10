"""ems/core/oam — SIP 통계 서비스축 판정 단위시험 (오프라인, DB/agent 없음).

sip_statistics.md §3.1: `access_services` 의 domain→kind 로 SIP 원문을 Request-URI → To → From 순으로 분류하고,
결과는 서비스축 `volte|ptt` 다 — 접속환경 kind `voip`(유선)는 전화 계열로 `volte` 에 합산한다
(`services.access_services.service_axis`, CSP `CCspServiceMap::LogServiceOf` 와 같은 규칙).

  python3 tests/test_oam_stats_classify.py
sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from services import access_services  # noqa: E402
from handlers import stats  # noqa: E402

DMAP = {"volte.example": "volte", "voip.example": "voip", "ptt.example": "ptt"}


def _req(method: str, ruri: str, to: str, frm: str) -> str:
    return (f"{method} {ruri} SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1\r\n"
            f"To: <{to}>\r\nFrom: <{frm}>;tag=1\r\nCall-ID: x\r\n\r\n")


def _rsp(to: str, frm: str) -> str:
    return f"SIP/2.0 200 OK\r\nTo: <{to}>;tag=2\r\nFrom: <{frm}>;tag=1\r\nCall-ID: x\r\n\r\n"


class ServiceAxisTests(unittest.TestCase):
    def test_phone_family_folds_to_volte(self):
        for k in ("volte", "voip", "VoIP", "VOLTE"):
            self.assertEqual(access_services.service_axis(k), "volte", k)

    def test_ptt_aliases(self):
        for k in ("ptt", "mcptt", "PTT"):
            self.assertEqual(access_services.service_axis(k), "ptt", k)

    def test_unknown_passes_through(self):
        self.assertEqual(access_services.service_axis("ibcf"), "ibcf")
        self.assertEqual(access_services.service_axis(""), "")
        self.assertEqual(access_services.service_axis(None), "")
        self.assertEqual(access_services.SERVICE_AXES, ("volte", "ptt"))


class ClassifyServiceTests(unittest.TestCase):
    def test_request_uri_first(self):
        msg = _req("INVITE", "sip:+8213@voip.example", "sip:+8213@voip.example", "sip:+8250@ptt.example")
        self.assertEqual(stats._classify_service(msg, DMAP), "volte")      # voip → 전화 계열 volte

    def test_voip_and_volte_share_axis(self):
        a = _req("REGISTER", "sip:voip.example", "sip:+8213@voip.example", "sip:+8213@voip.example")
        b = _req("REGISTER", "sip:volte.example", "sip:+8213@volte.example", "sip:+8213@volte.example")
        self.assertEqual(stats._classify_service(a, DMAP), stats._classify_service(b, DMAP))
        self.assertEqual(stats._classify_service(a, DMAP), "volte")

    def test_ptt_axis(self):
        msg = _req("INVITE", "sip:g001@ptt.example", "sip:g001@ptt.example", "sip:+8250@ptt.example")
        self.assertEqual(stats._classify_service(msg, DMAP), "ptt")

    def test_response_uses_to_then_from(self):
        self.assertEqual(stats._classify_service(_rsp("sip:+8213@voip.example", "sip:+8250@ptt.example"), DMAP), "volte")
        self.assertEqual(stats._classify_service(_rsp("sip:x@nowhere.example", "sip:+8250@ptt.example"), DMAP), "ptt")

    def test_unmatched_or_empty(self):
        self.assertEqual(stats._classify_service(_req("OPTIONS", "sip:10.0.0.1", "sip:10.0.0.1", "sip:10.0.0.1"), DMAP), "")
        self.assertEqual(stats._classify_service("", DMAP), "")
        self.assertEqual(stats._classify_service(_rsp("sip:+8213@voip.example", "sip:+8213@voip.example"), {}), "")


if __name__ == "__main__":
    unittest.main()
