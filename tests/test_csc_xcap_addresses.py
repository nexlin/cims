"""csc/src/services/mcptt.py — XCAP 문서 주소·이름(규격 주소와 CIMS 단말의 옛 주소 둘 다) 단위 시험 (오프라인, DB 없음).

TS 24.481 §6.2.2.2·§7.2.10.2(그룹 ID 로 찾는 문서 = global tree `byGroupID`) · §6.3.16(멤버를 뺀 조회 = POST + GMOP) ·
TS 24.484 §8.3.1A·§8.3.2.8(user profile 문서 이름) · §8.4.2.8·§8.4.2.9(service configuration = 전역 문서) · §7.2.1.1(UE initial
configuration 의 `<mcptt-UE-id>`) · §9.3.2.8·§9.4.2.8(MCVideo 문서 이름) · 쓰기 요청 405.

  python3 -m unittest tests.test_csc_xcap_addresses
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest
from urllib.parse import quote

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))
for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
    if os.path.isdir(_v) and _v not in sys.path:
        sys.path.append(_v)
        break

import services.mcptt as m  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

ALICE, BOB, CAROL = "tel:+82500000001", "tel:+82500000002", "tel:+82500000003"
GROUP = "tel:g-xa"
GMOP = ('<?xml version="1.0" encoding="UTF-8"?><document xmlns="urn:3gpp:ns:mcpttGMOP:1.0"><request>'
        '<{op}/></request></document>')


def _run(coro):
    return asyncio.run(coro)


class _Base(unittest.TestCase):
    def setUp(self):
        self._keep = (dict(m.GROUPS), m.extract_token, m.SCOPE_ENFORCEMENT, m._MCPTT_PUBLIC_URL, m.get_user_profile_xml,
                      m._mcvideo.get_user_profile_xml, dict(m.UE_INIT_CONFIG))
        m.GROUPS.clear()
        m.GROUPS[GROUP] = {"display_name": "xa", "priority": 5, "group_type": "prearranged", "etag": "etag_g-xa",
                           "members": [{"uri": ALICE, "name": "a"}, {"uri": BOB, "name": "b"}]}
        m.SCOPE_ENFORCEMENT, m._MCPTT_PUBLIC_URL = "enforce", "https://csc.example:4430"
        scope = " ".join([m.SCOPE_PTT_GMS, m.SCOPE_PTT_CMS, m.SCOPE_VIDEO_CMS])
        m.extract_token = lambda hdr: ({"mcptt_id": hdr.split()[-1], "scope": scope}
                                       if hdr and hdr.split()[-1].startswith("tel:") else None)
        m.get_user_profile_xml = lambda uri, owner_uid=None: (f"<up for='{uri}'/>", '"up-etag"')
        m._mcvideo.get_user_profile_xml = lambda uri: (f"<vup for='{uri}'/>", '"vup-etag"')

    def tearDown(self):
        (groups, m.extract_token, m.SCOPE_ENFORCEMENT, m._MCPTT_PUBLIC_URL, m.get_user_profile_xml,
         m._mcvideo.get_user_profile_xml, ue) = self._keep
        m.GROUPS.clear(); m.GROUPS.update(groups)
        m.UE_INIT_CONFIG.clear(); m.UE_INIT_CONFIG.update(ue)

    def call(self, handler, method, path, who=ALICE, body=None, headers=None):
        h = {"host": "10.0.0.9:4430"}
        if who:
            h["authorization"] = f"Bearer {who}"
        h.update(headers or {})
        return _run(handler(HandlerArgs(method, path, "127.0.0.1", 0, headers=h, body=body), {}))


class GroupByIdTest(_Base):
    BASE = "/org.openmobilealliance.groups/global/byGroupID/"

    def test_get_by_group_id_any_notation(self):
        via_users = self.call(m.handle_group_management, "GET", f"/org.openmobilealliance.groups/users/{ALICE}/{GROUP}")
        for ident in (GROUP, "g-xa", "sip:g-xa@ptt.example"):
            r = self.call(m.handle_group_by_id, "GET", self.BASE + quote(ident, safe=""))
            self.assertEqual(r.status, 200, ident)
            self.assertEqual(r.body, via_users.body, "users tree 의 문서와 같다")
            self.assertEqual(r.media_type, "application/vnd.oma.poc.groups+xml")
        self.assertEqual(self.call(m.handle_group_by_id, "GET", self.BASE + quote(GROUP, safe=""),
                                   headers={"if-none-match": r.headers["Etag"]}).status, 304)

    def test_authorisation_and_missing(self):
        self.assertEqual(self.call(m.handle_group_by_id, "GET", self.BASE + GROUP, who=CAROL).status, 403)
        self.assertEqual(self.call(m.handle_group_by_id, "GET", self.BASE + GROUP, who=None).status, 403)
        self.assertEqual(self.call(m.handle_group_by_id, "GET", self.BASE + "tel:g-none").status, 404)
        self.assertEqual(self.call(m.handle_group_by_id, "GET", "/org.openmobilealliance.groups/global/other/" + GROUP).status, 404)
        r = self.call(m.handle_group_by_id, "PUT", self.BASE + GROUP, body=b"<group/>")
        self.assertEqual((r.status, r.headers["Allow"]), (405, "GET, POST"))

    def test_post_gmop_excluding_members(self):
        full = self.call(m.handle_group_by_id, "GET", self.BASE + GROUP).body
        self.assertIn("<list>", full)
        r = self.call(m.handle_group_by_id, "POST", self.BASE + GROUP, body=GMOP.format(op="get-excluding-memberlist").encode(),
                      headers={"content-type": "application/vnd.3gpp.GMOP+xml; charset=utf-8"})
        self.assertEqual((r.status, r.media_type), (200, "application/vnd.oma.poc.groups+xml"))
        self.assertNotIn("<list>", r.body)
        self.assertNotIn("<entry", r.body)
        self.assertIn('<list-service uri="tel:g-xa">', r.body)
        self.assertIn("on-network-invite-members", r.body)               # 멤버 목록만 빠진다
        import xml.dom.minidom
        xml.dom.minidom.parseString(r.body.encode())                     # well-formed
        self.assertEqual(self.call(m.handle_group_by_id, "POST", self.BASE + GROUP, who=CAROL,
                                   body=GMOP.format(op="get-excluding-memberlist").encode(),
                                   headers={"content-type": "application/vnd.3gpp.GMOP+xml"}).status, 403)

    def test_post_rejections(self):
        gm = {"content-type": "application/vnd.3gpp.GMOP+xml"}
        self.assertEqual(self.call(m.handle_group_by_id, "POST", self.BASE + GROUP, body=b"<x/>",
                                   headers={"content-type": "application/xml"}).status, 415)
        self.assertEqual(self.call(m.handle_group_by_id, "POST", self.BASE + GROUP,
                                   body=GMOP.format(op="group-regroup-creation").encode(), headers=gm).status, 501)
        self.assertEqual(self.call(m.handle_group_by_id, "POST", self.BASE + GROUP, body=b"not xml", headers=gm).status, 400)
        self.assertEqual(self.call(m.handle_group_by_id, "POST", self.BASE + GROUP,
                                   body=b"<!DOCTYPE d><document/>", headers=gm).status, 400)


class CmsAddressTest(_Base):
    def test_service_config_global_and_legacy(self):
        legacy = self.call(m.handle_service_config, "GET", f"/org.3gpp.mcptt.service-config/users/{ALICE}/service-config")
        self.assertEqual(legacy.status, 200)
        for path in ("/org.3gpp.mcptt.service-config/global/service-config.xml",
                     "/org.3gpp.mcptt.service-config/global/mcorg1/service-config.xml"):
            r = self.call(m.handle_service_config_global, "GET", path)
            self.assertEqual((r.status, r.body, r.headers["Etag"]), (200, legacy.body, legacy.headers["Etag"]), path)
        g = "/org.3gpp.mcptt.service-config/global/"
        self.assertEqual(self.call(m.handle_service_config_global, "GET", g + "other.xml").status, 404)
        self.assertEqual(self.call(m.handle_service_config_global, "GET", g + "a/b/service-config.xml").status, 404)
        self.assertEqual(self.call(m.handle_service_config_global, "GET", g + "service-config.xml", who=None).status, 403)
        self.assertEqual(self.call(m.handle_service_config_global, "GET", g + "service-config.xml",
                                   headers={"if-none-match": legacy.headers["Etag"]}).status, 304)
        self.assertEqual(self.call(m.handle_service_config, "GET",
                                   f"/org.3gpp.mcptt.service-config/users/{BOB}/service-config").status, 403)

    def test_user_profile_document_names(self):
        base = "/org.3gpp.mcptt.user-profile/users/"
        for xui in (ALICE, "sip:+82500000001@ptt.example"):
            for name in ("mcptt-user-profile-1.xml", "user-profile"):
                r = self.call(m.handle_user_profile, "GET", base + quote(xui, safe="") + "/" + name)
                self.assertEqual((r.status, r.body), (200, f"<up for='{ALICE}'/>"), (xui, name))
        self.assertEqual(self.call(m.handle_user_profile, "GET", base + ALICE + "/mcptt-user-profile-2.xml").status, 404)
        self.assertEqual(self.call(m.handle_user_profile, "GET", base + ALICE).status, 404)
        self.assertEqual(self.call(m.handle_user_profile, "GET", base + BOB + "/mcptt-user-profile-1.xml").status, 403)
        self.assertEqual(self.call(m.handle_user_profile, "GET", base + ALICE + "/user-profile",
                                   headers={"if-none-match": '"up-etag"'}).status, 304)

    def test_write_methods_are_405_not_200(self):
        for handler, path in ((m.handle_user_profile, f"/org.3gpp.mcptt.user-profile/users/{ALICE}/mcptt-user-profile-1.xml"),
                              (m.handle_service_config, f"/org.3gpp.mcptt.service-config/users/{ALICE}/service-config"),
                              (m.handle_service_config_global, "/org.3gpp.mcptt.service-config/global/service-config.xml"),
                              (m.handle_mcvideo_user_profile, f"/org.3gpp.mcvideo.user-profile/users/{ALICE}/mcvideo-user-profile-1.xml"),
                              (m.handle_mcvideo_service_config, "/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml"),
                              (m.handle_ue_init_config, "/org.3gpp.mcptt.ue-init-config/users/sip:ue1/ue1")):
            for method in ("PUT", "DELETE", "POST"):
                r = self.call(handler, method, path, body=b"<x/>")
                self.assertEqual((r.status, r.headers.get("Allow")), (405, "GET"), (path, method))

    def test_mcvideo_document_names(self):
        up = "/org.3gpp.mcvideo.user-profile/users/"
        self.assertEqual(self.call(m.handle_mcvideo_user_profile, "GET", up + ALICE + "/mcvideo-user-profile-1.xml").status, 200)
        self.assertEqual(self.call(m.handle_mcvideo_user_profile, "GET", up + ALICE + "/mcvideo-user-profile-2.xml").status, 404)
        self.assertEqual(self.call(m.handle_mcvideo_user_profile, "GET", up + ALICE + "/whatever").status, 404)
        self.assertEqual(self.call(m.handle_mcvideo_user_profile, "GET", up + BOB + "/mcvideo-user-profile-1.xml").status, 403)
        # MCVideo UE configuration(§9.2.1A) — …/users/sip:<MCVideo ID>/<MCS UE ID>
        keep, seen = m._mcvideo.get_ue_config_xml, []
        m._mcvideo.get_ue_config_xml = lambda uri, ue_id='': (seen.append((uri, ue_id)) or ("<uec/>", '"uec"'))
        try:
            uc = "/org.3gpp.mcvideo.ue-config/users/"
            r = self.call(m.handle_mcvideo_ue_config, "GET", uc + quote("sip:+82500000001@ptt.example", safe="") + "/urn%3Auuid%3Aabc")
            self.assertEqual((r.status, r.media_type), (200, "application/vnd.3gpp.mcvideo-ue-config+xml"))
            self.assertEqual(seen[-1], (ALICE, "urn:uuid:abc"))
            self.assertEqual(self.call(m.handle_mcvideo_ue_config, "GET", uc + BOB + "/ue1").status, 403)
            self.assertEqual(self.call(m.handle_mcvideo_ue_config, "GET", uc + ALICE).status, 404)
            self.assertEqual(self.call(m.handle_mcvideo_ue_config, "PUT", uc + ALICE + "/ue1", body=b"<x/>").status, 405)
        finally:
            m._mcvideo.get_ue_config_xml = keep
        sc = "/org.3gpp.mcvideo.service-config/global/"
        self.assertEqual(self.call(m.handle_mcvideo_service_config, "GET", sc + "mcvideo-service-config.xml").status, 200)
        self.assertEqual(self.call(m.handle_mcvideo_service_config, "GET", sc + "mcorg1/mcvideo-service-config.xml").status, 200)
        self.assertEqual(self.call(m.handle_mcvideo_service_config, "GET", sc + "service-config.xml").status, 404)
        self.assertEqual(self.call(m.handle_mcvideo_service_config, "GET", sc).status, 404)


class UeInitConfigTest(_Base):
    def test_generated_document_carries_the_ue_id(self):
        ue = "urn:gsma:imei:35875810-123456-0"
        path = "/org.3gpp.mcptt.ue-init-config/users/" + quote("sip:" + ue, safe="") + "/" + quote(ue, safe="")
        r = self.call(m.handle_ue_init_config, "GET", path, who=None)             # 로그인 전 — 익명
        self.assertEqual(r.status, 200)
        self.assertIn(f"<mcptt-UE-id>\n    <Instance-ID-URN>{ue}</Instance-ID-URN>\n  </mcptt-UE-id>", r.body)
        self.assertLess(r.body.index("<mcptt-UE-id>"), r.body.index("<on-network>"))
        import xml.dom.minidom
        xml.dom.minidom.parseString(r.body.encode())
        other = self.call(m.handle_ue_init_config, "GET", "/org.3gpp.mcptt.ue-init-config/users/sip:ue2/ue2", who=None)
        self.assertIn("<Instance-ID-URN>ue2</Instance-ID-URN>", other.body)
        self.assertNotEqual(other.headers["Etag"], r.headers["Etag"], "문서가 다르면 ETag 도 다르다")
        self.assertEqual(self.call(m.handle_ue_init_config, "GET", path, who=None,
                                   headers={"if-none-match": r.headers["Etag"]}).status, 304)
        master = self.call(m.handle_ue_init_config, "GET", "/org.3gpp.mcptt.ue-init-config/users/sip:x", who=None)
        self.assertNotIn("mcptt-UE-id", master.body, "문서 이름(UE ID)이 없으면 master 그대로")

    def test_ue_id_is_escaped(self):
        r = self.call(m.handle_ue_init_config, "GET", "/org.3gpp.mcptt.ue-init-config/users/sip:x/" + quote("a<b&c", safe=""), who=None)
        self.assertIn("<Instance-ID-URN>a&lt;b&amp;c</Instance-ID-URN>", r.body)

    def test_http_proxy_defaults_to_public_base(self):
        r = self.call(m.handle_ue_init_config, "GET", "/org.3gpp.mcptt.ue-init-config/users/sip:x/x", who=None)
        self.assertIn("<http-proxy>https://csc.example:4430</http-proxy>", r.body)
        m.UE_INIT_CONFIG["HttpProxy"] = "https://proxy.example:8443"
        r = self.call(m.handle_ue_init_config, "GET", "/org.3gpp.mcptt.ue-init-config/users/sip:x/x", who=None)
        self.assertIn("<http-proxy>https://proxy.example:8443</http-proxy>", r.body)


if __name__ == "__main__":
    unittest.main()
