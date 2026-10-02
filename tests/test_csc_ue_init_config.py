"""csc/src/services/mcptt.py — MCS UE initial configuration 문서(TS 24.484 §7.2)의 단말 타이머·변경 통지 단위 시험 (오프라인).

mcptt_timers.md §7 D4·D10:
  · <on-network><Timers> 기본값이 TS 24.380 표 11.1.1-1 범위 안 — T100·T101 은 재전송 총 시간(× C100·C101 기본 3회)이 6초
    미만(NOTE 1 shall · NOTE 2 should)이 되는 1초, T103 = T1(4초), T104 = 사이트 값 4, T132 = 규격 기본 2초. 설정이 덮고
    xs:unsignedByte 범위로 자른다.
  · 재적재(apply_config 두 번째부터)로 문서 내용이 바뀌면 CSP 에 UE_INIT_CONFIG_CHANGED(etag = 새 문서 ETag)를 보낸다 —
    첫 적재(기동)·내용 불변 재적재는 보내지 않는다(TS 24.484 §7.2.2.12 → §6.3.13.3). 주소류(McpttServer.PublicUrl)만 바뀌어도
    문서가 바뀐다.
  · <HPLMN PLMN> = MCC 3 + MNC 2·3자리(TS 24.484 §7.2.2.7 · TS 23.003 §2.2) — 설정이 정본, 없으면 도메인 `mnc<3자리>.mcc<3자리>`
    에서 앞자리 0 하나만 뗀다(TS 23.003 §13 — 두 자리 MNC 는 0 을 하나 채워 쓴다).

  python3 -m unittest tests.test_csc_ue_init_config
"""
from __future__ import annotations

import os
import sys
import unittest
import xml.etree.ElementTree as ET

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))
# 의존성은 시스템이 아니라 **vendor** 에 있다(DEV_SERVER_SETUP.md — `pip install --target=csc/vendor`).
for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
    if os.path.isdir(_v) and _v not in sys.path:
        sys.path.append(_v)
        break

import services.access_services as acs  # noqa: E402
import services.mcptt as m  # noqa: E402
import services.mcvideo as mv  # noqa: E402

NS = {"ui": "urn:3gpp:mcptt:mcpttUEinitConfig:1.0"}
BASE = "https://csc.ptt.cims.example.kr:4430"


def _timers(xml: str) -> dict:
    t = ET.fromstring(xml.encode()).find("ui:on-network/ui:Timers", NS)
    return {c.tag.split("}")[1]: int(c.text) for c in t}


class UeInitTimersTest(unittest.TestCase):
    def setUp(self):
        self._keep = m.UE_INIT_CONFIG

    def tearDown(self):
        m.UE_INIT_CONFIG = self._keep

    def test_default_timers_within_spec(self):
        m.UE_INIT_CONFIG = {}
        t = _timers(m.get_ue_init_config_xml(BASE)[0])
        self.assertEqual(list(t), ["T100", "T101", "T103", "T104", "T132"], "§7.2.2.3 Timers 시퀀스 순서")
        self.assertEqual(t, {"T100": 1, "T101": 1, "T103": 4, "T104": 4, "T132": 2})
        self.assertLess(t["T100"] * 3, 6, "NOTE 1 — Floor Release 재전송 총 시간 6초 미만(C100 기본 3)")
        self.assertLess(t["T101"] * 3, 6, "NOTE 2 — Floor Request 재전송 총 시간 6초 미만 권장(C101 기본 3)")
        t1 = m._SERVICE_CONFIG_PARAM_DEFAULTS["FcTimersCounters"]["T1-end-of-rtp-media"]
        self.assertEqual(t["T103"] * 1000, t1, "T103 = T1 (표 11.1.1-1 «Should be equal to T1»)")

    def test_config_overrides_and_clamps(self):
        m.UE_INIT_CONFIG = {"Timers": {"T101": 2, "T132": "300", "T104": "x"}}
        t = _timers(m.get_ue_init_config_xml(BASE)[0])
        self.assertEqual(t, {"T100": 1, "T101": 2, "T103": 4, "T104": 4, "T132": 255},
                         "설정 반영 · unsignedByte 절단(300→255) · 정수 아님 = 기본값")


class UeInitHplmnTest(unittest.TestCase):
    def test_derive_from_domain_strips_one_padding_zero(self):
        # TS 23.003 §13 — 도메인의 MNC 는 세 자리, 두 자리 MNC 는 앞에 0 하나. 앞자리 0 을 전부 지우면 mnc008 → 4508(무효)
        self.assertEqual(m.derive_hplmn("", "ptt.mnc008.mcc450.3gppnetwork.org"), "45008")
        self.assertEqual(m.derive_hplmn("", "mnc033.mcc450.pub.3gppnetwork.org"), "45033")
        self.assertEqual(m.derive_hplmn("", "ims.mnc410.mcc310.3gppnetwork.org"), "310410", "세 자리 MNC")
        self.assertEqual(m.derive_hplmn("", "ptt.cims.example.kr"), "00101", "유도 불가 = 명목값")

    def test_configured_plmn_wins_and_is_validated(self):
        # 앞자리 0 인 세 자리 MNC(예 310-012)는 도메인으로 가를 수 없어 설정이 정본이다
        self.assertEqual(m.derive_hplmn("310012", "mnc012.mcc310.3gppnetwork.org"), "310012")
        self.assertEqual(m.derive_hplmn("45-08", "mnc008.mcc450.3gppnetwork.org"), "45008", "PLMN 코드가 아닌 설정 = 유도값")

    def test_document_carries_plmn(self):
        keep_cfg, keep_dom = m.UE_INIT_CONFIG, acs.ptt_domain
        try:
            m.UE_INIT_CONFIG = {}
            acs.ptt_domain = lambda provisioning, config=None: "ptt.mnc008.mcc450.3gppnetwork.org"
            hp = ET.fromstring(m.get_ue_init_config_xml(BASE + "/plmn")[0].encode()).find("ui:on-network/ui:HPLMN", NS)
            self.assertEqual(hp.get("PLMN"), "45008")
        finally:
            m.UE_INIT_CONFIG, acs.ptt_domain = keep_cfg, keep_dom
            m._UE_INIT_LAST_GOOD.pop(BASE + "/plmn", None)


class UeInitChangeNotifyTest(unittest.TestCase):
    """재적재 → UE_INIT_CONFIG_CHANGED (D10). apply_config 가 건드리는 모듈 전역은 시험 뒤 되돌린다."""

    def setUp(self):
        self._snap = {mod: dict(vars(mod)) for mod in (m, mv, acs)}
        self.sent = []
        m.notify_csp = lambda event, uri, action, etag="", **k: self.sent.append((event, uri, action, etag))
        m._UE_INIT_LOADED = False
        m._SERVICE_CONFIG_PARAMS_LOADED = False

    def tearDown(self):
        for mod, d in self._snap.items():
            for k, v in d.items():
                setattr(mod, k, v)

    @staticmethod
    def _cfg(**over):
        cfg = {"IdMs": {"JwtSecret": "unit", "Domain": "ptt.cims.example.kr"},
               "McpttServer": {"Port": 4430, "PublicUrl": BASE},
               "UeInitConfig": {"Timers": {"T101": 1}}}
        cfg.update(over)
        return cfg

    def _ui_events(self):
        return [e for e in self.sent if e[0] == "UE_INIT_CONFIG_CHANGED"]

    # user profile 기본값(UserProfile.*) 재적재 → USER_PROFILE_CONFIG_CHANGED(TS 24.484 §8.3.2.12 — cms 구독자 전원의 user profile 통지 계기)
    def test_user_profile_defaults_change_notifies(self):
        m._USER_PROFILE_CONFIG_LOADED = False
        up = lambda: [e for e in self.sent if e[0] == "USER_PROFILE_CONFIG_CHANGED"]
        m.apply_config(self._cfg(UserProfile={"AllowPrivateCall": True}))
        self.assertEqual(up(), [], "첫 적재(기동)는 통지하지 않는다")
        m.apply_config(self._cfg(UserProfile={"AllowPrivateCall": True}))
        self.assertEqual(up(), [], "내용이 같으면 통지하지 않는다")
        m.apply_config(self._cfg(UserProfile={"AllowPrivateCall": False}))
        self.assertEqual(up(), [("USER_PROFILE_CONFIG_CHANGED", "", "PUT", "")])
        m.apply_config(self._cfg())                                      # 절을 지워 기본값으로 — 역시 바뀐 것
        self.assertEqual(len(up()), 2)

    def test_first_load_and_unchanged_reload_do_not_notify(self):
        m.apply_config(self._cfg())
        self.assertEqual(self._ui_events(), [], "첫 적재(기동)는 통지하지 않는다")
        m.apply_config(self._cfg())
        self.assertEqual(self._ui_events(), [], "내용이 같으면 통지하지 않는다")

    def test_timer_change_notifies_with_new_etag(self):
        m.apply_config(self._cfg())
        m.apply_config(self._cfg(UeInitConfig={"Timers": {"T101": 2}}))
        ev = self._ui_events()
        self.assertEqual(len(ev), 1)
        etag = m.get_ue_init_config_xml(BASE)[1].strip('"')
        self.assertEqual(ev[0], ("UE_INIT_CONFIG_CHANGED", "", "PUT", etag), "etag = 단말이 받을 새 문서의 ETag")

    def test_public_url_change_notifies(self):
        m.apply_config(self._cfg())
        m.apply_config(self._cfg(McpttServer={"Port": 4430, "PublicUrl": "https://csc2.ptt.cims.example.kr:4430"}))
        self.assertEqual(len(self._ui_events()), 1, "주소류(XCAP root·IdMS 주소)도 문서 내용이다")


if __name__ == "__main__":
    unittest.main()
