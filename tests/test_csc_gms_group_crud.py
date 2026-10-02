"""csc/src/services/mcptt.py — GMS XCAP 그룹 CRUD(가입자 주체) 단위 시험 (오프라인, DB 없음).

mcptt_authorization.md §3 / TS 24.481 Ut PUT·DELETE: 생성 = 프로파일 allow_create_group, 수정·삭제 = 소유
(authorized_user_id == 토큰 가입자 users.id). 본문 = get_group_xml 이 내는 문서와 같은 포맷.
DB 쓰기(gms_write_group)는 가짜 커넥션으로 SQL 조립을 검증한다.

  python3 -m unittest tests.test_csc_gms_group_crud
"""
from __future__ import annotations

import asyncio
import os
import re
import sys
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

import services.mcptt as m  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

PTT_DOM = "ptt.cims.example.kr"
OWNER_LOGIN, OWNER_UID, OWNER_PTT = "disp01", 5020, "+82510001001"
OTHER_LOGIN, OTHER_UID, OTHER_PTT = "disp02", 5021, "+82510001002"


def _args(method, path, body=None, headers=None):
    h = {"authorization": "Bearer x"}
    h.update(headers or {})
    return HandlerArgs(method, path, "127.0.0.1", 0, headers=h, body=body)


def _run(coro):
    return asyncio.run(coro)


def _doc(uri, name, members, group_type="prearranged", extra=""):
    entries = "".join(
        f'<entry uri="{u}"><rl:display-name>{u}</rl:display-name>'
        f'<mcpttgi:participant-type>{r}</mcpttgi:participant-type>'
        f'<mcpttgi:user-priority>{p}</mcpttgi:user-priority></entry>' for u, r, p in members)
    return (f'<?xml version="1.0" encoding="UTF-8"?><group xmlns="urn:oma:xml:poc:list-service" '
            f'xmlns:rl="urn:ietf:params:xml:ns:resource-lists" xmlns:cp="urn:ietf:params:xml:ns:common-policy" '
            f'xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"><list-service uri="{uri}">'
            f'<display-name>{name}</display-name><list>{entries}</list>'
            f'<mcpttgi:on-network-invite-members>{"false" if group_type == "chat" else "true"}'
            f'</mcpttgi:on-network-invite-members>{extra}</list-service></group>')


class _Base(unittest.TestCase):
    def setUp(self):
        self._saved = (dict(m.GROUPS), dict(m.LOGIN_ACCOUNTS), dict(m.PTT_PROFILES), m._DB_CONFIG,
                       m.extract_token, m.notify_csp, m.save_group_to_file, m.delete_group_file)
        m.GROUPS.clear(); m.LOGIN_ACCOUNTS.clear(); m.PTT_PROFILES.clear()
        m._DB_CONFIG = None                      # 파일 폴백 경로 — DB 없이 인가·파싱만 검증
        m.notify_csp = lambda *a, **k: None
        m.save_group_to_file = lambda *a, **k: None
        m.delete_group_file = lambda *a, **k: None
        m.LOGIN_ACCOUNTS[OWNER_LOGIN] = {"user_id": OWNER_UID, "mcptt_id": f"tel:{OWNER_PTT}", "password": "", "name": "관제1석"}
        m.LOGIN_ACCOUNTS[OTHER_LOGIN] = {"user_id": OTHER_UID, "mcptt_id": f"tel:{OTHER_PTT}", "password": "", "name": "관제2석"}
        # 실토큰처럼 GMS scope 를 싣는다(TS 33.180 B.10 — handle_group_management 가 require_scope 로 검사).
        self.token = {"sub": OWNER_LOGIN, "mcptt_id": f"tel:{OWNER_PTT}", "scope": m.SCOPE_PTT_GMS}
        m.extract_token = lambda hdr: self.token if hdr else None

    def tearDown(self):
        g, la, pp, db, et, nc, sg, dg = self._saved
        m.GROUPS.clear(); m.GROUPS.update(g)
        m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(la)
        m.PTT_PROFILES.clear(); m.PTT_PROFILES.update(pp)
        m._DB_CONFIG, m.extract_token, m.notify_csp, m.save_group_to_file, m.delete_group_file = db, et, nc, sg, dg

    def _grant_create(self, ptt=OWNER_PTT):
        m.PTT_PROFILES[ptt] = dict(m.DEFAULT_USER_PROFILE, allow_create_group=True)

    def _existing(self, gid, owner_uid, members=()):
        m.GROUPS[f"tel:{gid}"] = {
            "display_name": gid, "etag": f"etag_{gid}", "authorized_user_id": owner_uid,
            "authorized_user": "", "members": [{"uri": f"tel:{u}", "name": u, "role": "participant", "priority": 0}
                                                for u in members],
        }

    def _put(self, gid, xml, xui=OWNER_PTT, headers=None):
        return _run(m.handle_group_management(
            _args("PUT", f"/org.openmobilealliance.groups/users/tel:{xui}/tel:{gid}",
                  body=xml.encode(), headers=headers), {}))

    def _delete(self, gid, xui=OWNER_PTT):
        return _run(m.handle_group_management(
            _args("DELETE", f"/org.openmobilealliance.groups/users/tel:{xui}/tel:{gid}"), {}))


class ParseTests(unittest.TestCase):
    def test_roundtrip_with_get_group_xml(self):
        m.GROUPS["tel:g-0000abcd"] = {
            "display_name": "관제채널", "etag": "e", "priority": 3, "encryption": False,
            "emergency_call": True, "emergency_alert": False, "allow_conference_state": False, "allow_sds": True,
            "allow_fd": True, "max_sds_size": 2000, "max_auto_recv": 4096, "org_code": "TEAM01", "group_type": "chat",
            "max_members": 7, "require_affiliation": False, "authorized_user": "tel:+82510001001",
            "hang_timer_sec": 5, "max_duration_sec": 600,
            "members": [{"uri": "tel:+82510001001", "name": "관제1석", "role": "chair", "priority": 1, "title": "팀장"},
                        {"uri": "tel:+82500000001", "name": "테스트001", "role": "participant", "priority": 5}],
        }
        try:
            xml, _ = m.get_group_xml("tel:g-0000abcd")
            d = m.parse_group_document_xml(xml)
        finally:
            m.GROUPS.pop("tel:g-0000abcd", None)
        self.assertEqual(d["display_name"], "관제채널")
        self.assertEqual(d["group_type"], "chat")
        self.assertEqual((d["priority"], d["encryption"]), (3, False))
        self.assertEqual((d["emergency_call"], d["emergency_alert"]), (True, False))
        # on-network-allow-conference-state (TS 24.481 §7.2.4.2) — cp:actions 요소 왕복
        self.assertIs(d["allow_conference_state"], False)
        self.assertIn("<mcpttgi:on-network-allow-conference-state>false</mcpttgi:on-network-allow-conference-state>", xml)
        self.assertEqual((d["allow_sds"], d["allow_fd"], d["max_sds_size"], d["max_auto_recv"]), (True, True, 2000, 4096))
        self.assertEqual((d["max_members"], d["require_affiliation"], d["org_code"]), (7, False, "TEAM01"))
        # 그룹 종류 = on-network-invite-members (TS 24.481 §7.2.2 a), 호 타이머 = xs:duration 규격 요소명
        self.assertIn("<mcpttgi:on-network-invite-members>false</mcpttgi:on-network-invite-members>", xml)
        self.assertNotIn("session-type", xml)   # 규격 밖 요소 — 그룹 문서에 싣지 않는다
        self.assertIn("<mcpttgi:on-network-hang-timer>PT5S</mcpttgi:on-network-hang-timer>", xml)
        self.assertNotIn("on-network-hang-time>", xml)
        # chat 그룹은 TNG3 를 돌리지 않는다 — 요소를 싣지 않고(mcptt_timers.md §7 D7), PUT 으로 되읽으면 «기존값 유지»
        self.assertNotIn("on-network-maximum-duration", xml)
        self.assertEqual((d["hang_timer_sec"], d["max_duration_sec"]), (5, None))
        self.assertEqual([(x["user_id"], x["role"], x["priority"]) for x in d["members"]],
                         [("+82510001001", "chair", 1), ("+82500000001", "participant", 5)])

    def test_call_timers_zero_and_chat(self):
        """그룹 호 타이머의 0(TS 24.481 §7.2.2 o)p)·§7.2.7, mcptt_timers.md §7 D7·D8) — T4 0 = 요소 생략, 편성 그룹 TNG3 0 =
        무제한 표기(값 필수), chat 그룹 = TNG3 요소 없음. GET 문서를 그대로 PUT 해도 값이 바뀌지 않는다."""
        tag_t4, tag_tng3 = "on-network-hang-timer", "on-network-maximum-duration"
        cases = (  # (그룹 종류, T4, TNG3) → (T4 요소, TNG3 요소, 되읽은 T4, 되읽은 TNG3)
            ("prearranged", 0, 0, None, f"PT{m.GROUP_MAX_DURATION_UNLIMITED}S", None, 0),
            ("prearranged", 30, 3600, "PT30S", "PT3600S", 30, 3600),
            ("prearranged", 0, 86400, None, "PT86400S", None, 86400),
            ("chat", 0, 0, None, None, None, None),
            ("chat", 45, 3600, "PT45S", None, 45, None),
        )
        for gt, t4, tng3, want_t4, want_tng3, back_t4, back_tng3 in cases:
            m.GROUPS["tel:g-0000t4t3"] = {"display_name": "t", "etag": "e", "group_type": gt,
                                          "hang_timer_sec": t4, "max_duration_sec": tng3, "members": []}
            try:
                xml, _ = m.get_group_xml("tel:g-0000t4t3")
            finally:
                m.GROUPS.pop("tel:g-0000t4t3", None)
            got = {t: re.search(rf"<mcpttgi:{t}>([^<]*)</mcpttgi:{t}>", xml) for t in (tag_t4, tag_tng3)}
            self.assertEqual(got[tag_t4] and got[tag_t4].group(1), want_t4, (gt, t4, tng3))
            self.assertEqual(got[tag_tng3] and got[tag_tng3].group(1), want_tng3, (gt, t4, tng3))
            self.assertNotIn("PT0S", xml, "0 은 문서에 싣지 않는다(규격 단말이 «0초» 로 읽는다)")
            d = m.parse_group_document_xml(xml)
            self.assertEqual((d["hang_timer_sec"], d["max_duration_sec"]), (back_t4, back_tng3), (gt, t4, tng3))
        # 무제한 표기 이상(다른 xs:duration 표기 포함)은 0, 설정 범위 밖 유한값은 400 그대로, PT0S 는 0 으로 받는다
        for text, want in (("PT2147483647S", 0), ("P24855DT3H14M7S", 0), ("PT0S", 0), ("PT600S", 600)):
            self.assertEqual(m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=(
                f"<mcpttgi:{tag_tng3}>{text}</mcpttgi:{tag_tng3}>")))["max_duration_sec"], want, text)
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=(
                f"<mcpttgi:{tag_tng3}>PT86401S</mcpttgi:{tag_tng3}>")))
        self.assertEqual(m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=(
            f"<mcpttgi:{tag_t4}>PT0S</mcpttgi:{tag_t4}>")))["hang_timer_sec"], 0, "명시한 PT0S = T4 미사용")

    def test_ack_call_setup_elements(self):
        """확인 통화 설정 (TS 24.481 §7.2.2 s)t)u)·§7.2.4.2) — 필수 멤버만 <on-network-required>, list-service 3 요소 왕복."""
        m.GROUPS["tel:g-0000ack1"] = {
            "display_name": "필수", "etag": "e", "group_type": "prearranged",
            "min_number_to_start": 2, "ack_timeout_sec": 7, "ack_action": "proceed",
            "members": [{"uri": "tel:+82500000001", "name": "a", "role": "participant", "priority": 1, "required": True},
                        {"uri": "tel:+82500000002", "name": "b", "role": "participant", "priority": 2}],
        }
        try:
            xml, _ = m.get_group_xml("tel:g-0000ack1")
            d = m.parse_group_document_xml(xml)
        finally:
            m.GROUPS.pop("tel:g-0000ack1", None)
        self.assertEqual(xml.count("<mcpttgi:on-network-required/>"), 1)   # 필수 아닌 멤버엔 싣지 않는다
        self.assertIn("<mcpttgi:on-network-minimum-number-to-start>2</mcpttgi:on-network-minimum-number-to-start>", xml)
        self.assertIn("<mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>PT7S"
                      "</mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>", xml)
        self.assertIn("<mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>"
                      "proceed</mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>",
                      xml)
        self.assertEqual((d["min_number_to_start"], d["ack_timeout_sec"], d["ack_action"]), (2, 7, "proceed"))
        self.assertEqual([x["required"] for x in d["members"]], [True, False])
        # 정의 밖 동작 값 = abandon (§7.2.2 u)), 범위 밖 TNG1·unsignedShort 아님은 거절
        tag = ("<mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>{}"
               "</mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>")
        self.assertEqual(m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=tag.format("later")))
                         ["ack_action"], "abandon")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=(
                "<mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>PT0S"
                "</mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>")))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=(
                "<mcpttgi:on-network-minimum-number-to-start>-1</mcpttgi:on-network-minimum-number-to-start>")))

    def test_group_type_from_invite_members_only(self):
        d = m.parse_group_document_xml(_doc("tel:g-00000001", "n", []))
        self.assertEqual(d["group_type"], "prearranged")
        d = m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], group_type="chat"))
        self.assertEqual(d["group_type"], "chat")
        # 규격 밖 <session-type> 은 읽지 않는다 — invite-members 가 없으면 그룹 종류 불변(None)
        d = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">'
            '<list-service uri="tel:g-00000001"><display-name>n</display-name>'
            '<mcpttgi:session-type>chat</mcpttgi:session-type></list-service></group>')
        self.assertIsNone(d["group_type"])

    def test_hang_timer_xs_duration(self):
        for text, want in (("PT30S", 30), ("PT1M", 60), ("PT1M30S", 90), ("PT2.5S", 2), ("45", 45), ("P0D", 0)):
            self.assertEqual(m.parse_xs_duration(text), want, text)
        for text in ("P", "PT", "30s", "-PT1S", "PTXS"):
            self.assertIsNone(m.parse_xs_duration(text), text)
        tag = "<mcpttgi:on-network-hang-timer>{}</mcpttgi:on-network-hang-timer>"
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=tag.format("soon")))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], extra=tag.format("PT3601S")))

    def test_missing_elements_are_none_and_members_absent(self):
        d = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service"><list-service uri="tel:g-00000001">'
            '<display-name>n</display-name></list-service></group>')
        self.assertEqual(d["display_name"], "n")
        self.assertIsNone(d["members"])
        self.assertIsNone(d["group_type"])
        self.assertIsNone(d["priority"])

    def test_rejects_malformed_dtd_oversize_bad_enums(self):
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<group><list-service>")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml('<!DOCTYPE x [<!ENTITY a "b">]><group/>')
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<a>" + "x" * (m._GMS_MAX_BODY + 1) + "</a>")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [("tel:+82500000001", "boss", 1)]))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<group xmlns=\"urn:oma:xml:poc:list-service\"/>")

    def test_member_uri_forms_normalize_to_msisdn(self):
        d = m.parse_group_document_xml(_doc("tel:g-00000001", "n", [
            ("tel:+82500000001", "participant", 0), (f"sip:+82500000002@{PTT_DOM}", "chair", 2), ("82500000003", "participant", 1)]))
        self.assertEqual([x["user_id"] for x in d["members"]], ["+82500000001", "+82500000002", "+82500000003"])
        self.assertEqual(d["members"][0]["mcptt_id"], "tel:+82500000001")
        self.assertIsNone(d["members"][2]["mcptt_id"])


class GroupDocValuesTests(unittest.TestCase):
    """그룹 문서 값(TS 24.481 §7.2.2·§7.2.8) — C01: GMS-7 보호 false · GMS-11 정원 0 생략 · GMS-12 명단 열람 · GMS-13 escape ·
    GMS-18 priorityType · GCC-10 선호 음성 코덱 · GCS-19 on-network-disabled."""
    GID = "tel:g-0000c001"
    NS = {"poc": "urn:oma:xml:poc:list-service", "gi": "urn:3gpp:ns:mcpttGroupInfo:1.0",
          "cp": "urn:ietf:params:xml:ns:common-policy", "cims": "urn:cims:groupinfo:1.0"}

    def setUp(self):
        m.GROUPS[self.GID] = {
            "display_name": "A&B <팀>", "etag": "e", "priority": 300, "encryption": False, "emergency_call": True,
            "emergency_alert": True, "allow_conference_state": True, "allow_sds": True, "allow_fd": False,
            "max_sds_size": 0, "max_auto_recv": 0, "org_code": "R&D", "group_type": "prearranged", "max_members": 0,
            "require_affiliation": True, "on_network": True, "authorized_user": "",
            "members": [{"uri": "tel:+82510001001", "name": "홍&길동 <1>", "role": "chair", "priority": -4, "title": "팀장 \"A\""}],
        }

    def tearDown(self):
        m.GROUPS.pop(self.GID, None)

    def _ls(self):
        import xml.etree.ElementTree as ET
        xml, _ = m.get_group_xml(self.GID)
        return xml, ET.fromstring(xml.encode()).find("poc:list-service", self.NS)

    def test_names_are_escaped_well_formed(self):
        xml, ls = self._ls()                                   # fromstring 이 깨지지 않는다 = well-formed (RFC 4825)
        self.assertEqual(ls.find("poc:display-name", self.NS).text, "A&B <팀>")
        e = ls.find("poc:list/poc:entry", self.NS)
        self.assertEqual(e.find("{urn:ietf:params:xml:ns:resource-lists}display-name").text, "홍&길동 <1>")
        self.assertEqual(e.find("cims:user-title", self.NS).text, '팀장 "A"')
        self.assertEqual(ls.find("cims:org-code", self.NS).text, "R&D")
        self.assertIsNone(ls.find("gi:org-code", self.NS))            # 자체 요소는 3GPP 이름공간에 두지 않는다(GMS-10)

    def test_protection_false_and_preferred_voice(self):
        _, ls = self._ls()
        # 보호 둘은 없으면 true(GMK 필수) — false 명시(§7.2.8)
        self.assertEqual(ls.find("gi:protect-media", self.NS).text, "false")
        self.assertEqual(ls.find("gi:protect-floor-control-signalling", self.NS).text, "false")
        # 선호 음성 코덱 = 서버 서비스 코덱(TS 24.379 §6.2.1 2)b) — 단말 offer 가 따른다)
        enc = ls.findall("gi:preferred-voice-encodings/gi:encoding", self.NS)
        self.assertEqual([x.get("name") for x in enc], [m.SERVICE_VOICE_ENCODING])
        self.assertEqual(m.SERVICE_VOICE_ENCODING, "AMR-WB")

    def test_unlimited_participants_omits_count(self):
        _, ls = self._ls()
        self.assertIsNone(ls.find("gi:on-network-max-participant-count", self.NS), "0 = 무제한 → 요소 생략(10 을 싣지 않는다)")
        m.GROUPS[self.GID]["max_members"] = 12
        _, ls = self._ls()
        self.assertEqual(ls.find("gi:on-network-max-participant-count", self.NS).text, "12")

    def test_member_list_rule_and_priority_clamp(self):
        _, ls = self._ls()
        acts = ls.find("cp:ruleset/cp:rule/cp:actions", self.NS)
        self.assertEqual(acts.find("gi:on-network-allow-getting-member-list", self.NS).text, "true",
                         "없으면 false — 멤버가 명단을 읽는 규칙(§7.2.12.1)")
        self.assertEqual(ls.find("gi:on-network-group-priority", self.NS).text, "255", "저장값 300 → priorityType 상한")
        self.assertEqual(ls.find("poc:list/poc:entry/gi:user-priority", self.NS).text, "0", "저장값 -4 → 하한")

    def test_on_network_disabled_roundtrip(self):
        _, ls = self._ls()
        self.assertIsNone(ls.find("gi:on-network-disabled", self.NS))
        m.GROUPS[self.GID]["on_network"] = False
        xml, ls = self._ls()
        self.assertIsNotNone(ls.find("gi:on-network-disabled", self.NS), "§7.2.2 g) — 꺼진 그룹 표시")
        self.assertIs(m.parse_group_document_xml(xml)["on_network"], False)
        m.GROUPS[self.GID]["on_network"] = True
        xml, _ = self._ls()
        self.assertIsNone(m.parse_group_document_xml(xml)["on_network"], "요소 없음 = 기존값 유지")

    def test_put_rejects_out_of_range_priorities(self):
        bad_member = _doc("tel:g-00000001", "n", [("tel:+82500000001", "participant", 256)])
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(bad_member)
        bad_group = _doc("tel:g-00000001", "n", [("tel:+82500000001", "participant", 255)],
                         extra="<mcpttgi:on-network-group-priority>999</mcpttgi:on-network-group-priority>")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(bad_group)
        ok = m.parse_group_document_xml(_doc("tel:g-00000001", "n", [("tel:+82500000001", "participant", 255)],
                                             extra="<mcpttgi:on-network-group-priority>0</mcpttgi:on-network-group-priority>"))
        self.assertEqual((ok["priority"], ok["members"][0]["priority"]), (0, 255))


class IdTests(unittest.TestCase):
    def test_validate_new_group_id(self):
        self.assertIsNone(m.validate_new_gms_group_id("g-0a1b2c3d"))
        self.assertIn("reserved", m.validate_new_gms_group_id("adhoc-x"))
        self.assertIn("reserved", m.validate_new_gms_group_id("priv-x"))
        self.assertIn("8 lowercase hex", m.validate_new_gms_group_id("g001"))
        self.assertIn("8 lowercase hex", m.validate_new_gms_group_id("g-0A1B2C3D"))
        self.assertIn("required", m.validate_new_gms_group_id(""))
        self.assertIn("not the PTT domain", m.validate_new_gms_group_id("g-0a1b2c3d", "example.com"))
        self.assertIsNone(m.validate_new_gms_group_id("g-0a1b2c3d", m.IDMS_DOMAIN))

    def test_gid_from_uri(self):
        self.assertEqual(m._gms_gid_from_uri("tel:g-0a1b2c3d"), ("g-0a1b2c3d", ""))
        self.assertEqual(m._gms_gid_from_uri(f"sip:g-0a1b2c3d@{PTT_DOM}"), ("g-0a1b2c3d", PTT_DOM))
        self.assertEqual(m._gms_gid_from_uri("g001"), ("g001", ""))

    def test_token_user_id_and_ptt_id(self):
        saved = dict(m.LOGIN_ACCOUNTS)
        try:
            m.LOGIN_ACCOUNTS["disp01"] = {"user_id": 5020}
            self.assertEqual(m._token_user_id({"sub": "disp01"}), 5020)
            self.assertIsNone(m._token_user_id({"sub": "nobody"}))
            self.assertIsNone(m._token_user_id({}))
        finally:
            m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(saved)
        self.assertEqual(m._requester_ptt_id({"mcptt_id": "tel:+82510001001"}), "+82510001001")
        self.assertEqual(m._requester_ptt_id({"mcptt_id": f"sip:+82510001001@{PTT_DOM}"}), "+82510001001")


class GateTests(_Base):
    def test_put_new_requires_allow_create_group(self):
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []))
        self.assertEqual(r.status, 403)
        self.assertIn("group_creation_not_allowed", r.body)

    def test_put_new_rejects_bad_id_even_with_grant(self):
        self._grant_create()
        self.assertEqual(self._put("g001", _doc("tel:g001", "n", [])).status, 400)
        r = self._put("adhoc-1", _doc("tel:adhoc-1", "n", []))
        self.assertEqual(r.status, 400)
        self.assertIn("reserved_prefix", r.body)

    def test_put_new_creates_with_owner_201(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "관제채널", [("tel:+82500000001", "participant", 0)]))
        self.assertEqual(r.status, 201, r.body)
        self.assertIn("Etag", r.headers)
        g = m.GROUPS["tel:g-0a1b2c3d"]
        self.assertEqual(g["display_name"], "관제채널")
        self.assertEqual(g["authorized_user_id"], OWNER_UID)
        self.assertEqual([x["uri"] for x in g["members"]], ["tel:+82500000001"])

    def test_put_existing_owner_updates_200(self):
        self._existing("g-0a1b2c3d", OWNER_UID, members=["+82500000001"])
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "새이름", [("tel:+82500000002", "chair", 1)]))
        self.assertEqual(r.status, 200, r.body)
        g = m.GROUPS["tel:g-0a1b2c3d"]
        self.assertEqual(g["display_name"], "새이름")
        self.assertEqual([x["uri"] for x in g["members"]], ["tel:+82500000002"])

    def test_put_existing_other_owner_409_ownerless_403(self):
        self._existing("g-0a1b2c3d", OTHER_UID)
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []))
        self.assertEqual(r.status, 409)
        self.assertIn("uri_taken", r.body)
        self._existing("g001", None)
        r = self._put("g001", _doc("tel:g001", "n", []))
        self.assertEqual(r.status, 403)
        self.assertIn("not_group_owner", r.body)

    def test_put_if_match_mismatch_412(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []), headers={"if-match": '"stale"'})
        self.assertEqual(r.status, 412)

    def test_put_invalid_document_400(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", "<group>")
        self.assertEqual(r.status, 400)
        self.assertIn("invalid_group_document", r.body)

    def test_delete_owner_200_other_403_missing_404(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        self.assertEqual(self._delete("g-0a1b2c3d").status, 200)
        self.assertNotIn("tel:g-0a1b2c3d", m.GROUPS)
        self._existing("g-0a1b2c3e", OTHER_UID)
        r = self._delete("g-0a1b2c3e")
        self.assertEqual(r.status, 403)
        self.assertIn("tel:g-0a1b2c3e", m.GROUPS)
        self.assertEqual(self._delete("g-ffffffff").status, 404)

    def test_tree_owner_mismatch_403_and_no_token_403(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []), xui=OTHER_PTT)
        self.assertEqual(r.status, 403)
        r = _run(m.handle_group_management(
            HandlerArgs("PUT", f"/org.openmobilealliance.groups/users/tel:{OWNER_PTT}/tel:g-0a1b2c3d", "127.0.0.1", 0, headers={}), {}))
        self.assertEqual(r.status, 403, "Bearer 토큰이 없는 요청 = 403 (TS 24.482 A.2.3 1))")

    def test_list_includes_owner_and_marks_is_owner(self):
        import json
        self._existing("g-0a1b2c3d", OWNER_UID)                       # 소유자이나 비멤버
        self._existing("g001", None, members=[OWNER_PTT])            # 멤버이나 콘솔 그룹(소유자 없음)
        self._existing("g002", OTHER_UID)                            # 무관
        r = _run(m.handle_group_management(
            _args("GET", f"/org.openmobilealliance.groups/users/tel:{OWNER_PTT}"), {}))
        self.assertEqual(r.status, 200)
        rows = {x["uri"]: x["is_owner"] for x in json.loads(r.body)}
        self.assertEqual(rows, {"tel:g-0a1b2c3d": True, "tel:g001": False})


class _FakeCursor:
    def __init__(self, known_subs, existing_pk=None):
        self.sql, self.known, self.pk = [], set(known_subs), existing_pk
        self.lastrowid, self.rowcount, self._rows = 77, 0, []

    def execute(self, q, args=None):
        self.sql.append((q, tuple(args) if args is not None else ()))
        if q.startswith("SELECT id FROM ptt_subscriptions"):
            self._rows = [{"id": a} for a in args if a in self.known]
        elif q.startswith("SELECT id FROM ptt_groups"):
            self._rows = [{"id": self.pk}] if self.pk else []
        elif q.startswith("DELETE FROM ptt_groups"):
            self.rowcount = 1 if self.pk else 0
        elif q.startswith("SELECT g.max_members, (SELECT COUNT(*)"):
            self._rows = [self.group_row] if getattr(self, "group_row", None) else []
        else:
            self._rows = []

    def fetchall(self): return list(self._rows)
    def fetchone(self): return self._rows[0] if self._rows else None
    def __enter__(self): return self
    def __exit__(self, *a): return False


class _FakeConn:
    def __init__(self, cur): self.cur, self.committed = cur, False
    def cursor(self): return self.cur
    def commit(self): self.committed = True
    def __enter__(self): return self
    def __exit__(self, *a): return False


class XcapWriteTests(_Base):
    """C05 — 규칙·이름공간 해석, XCAP 오류 형식(RFC 4825 §11), 그룹 생성 XUI(TS 24.481 §6.3.2)."""
    XE = {"accept": "application/xcap-error+xml, */*"}

    def _create(self, uri, name="n", doc_name="new-group.xml", headers=None, body=None):
        self._keep_pub = m._MCPTT_PUBLIC_URL
        m._MCPTT_PUBLIC_URL = "https://csc.example:4430"
        try:
            xml = body if body is not None else _doc(uri, name, [(f"tel:{OWNER_PTT}", "chair", 1)])
            return _run(m.handle_group_management(
                _args("PUT", "/org.openmobilealliance.groups/users/https://csc.example:4430/" + doc_name,
                      body=xml.encode(), headers=headers), {}))
        finally:
            m._MCPTT_PUBLIC_URL = self._keep_pub

    # GMS-9 — 담을 수 없는 규칙은 받지 않는다(전 멤버 허용으로 넓혀 저장하지 않는다)
    def test_identity_rule_and_join_handling_false_are_rejected(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        ident = ('<cp:ruleset><cp:rule id="r1"><cp:conditions><cp:identity><cp:one id="tel:+82500000001"/></cp:identity>'
                 '</cp:conditions><cp:actions><mcpttgi:allow-MCPTT-emergency-call>true</mcpttgi:allow-MCPTT-emergency-call>'
                 '</cp:actions></cp:rule></cp:ruleset>')
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", [], extra=ident))
        self.assertEqual(r.status, 400)
        self.assertIn("identities", r.body)
        nojoin = ('<cp:ruleset><cp:rule id="r1"><cp:conditions><is-list-member/></cp:conditions><cp:actions>'
                  '<join-handling>false</join-handling></cp:actions></cp:rule></cp:ruleset>')
        self.assertEqual(self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", [], extra=nojoin)).status, 400)
        ok = nojoin.replace("false", "true")
        self.assertEqual(self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", [], extra=ok)).status, 200)

    # GMS-4 — XCAP 클라이언트에는 409 + application/xcap-error+xml, CIMS 앱에는 종전 JSON
    def test_document_errors_as_xcap_error_when_asked(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        bad = _doc("tel:g-0a1b2c3d", "n", [("tel:+82500000001", "boss", 0)])
        r = self._put("g-0a1b2c3d", bad)
        self.assertEqual((r.status, r.media_type), (400, "application/json"))
        r = self._put("g-0a1b2c3d", bad, headers=self.XE)
        self.assertEqual((r.status, r.media_type), (409, "application/xcap-error+xml"))
        self.assertIn('<xcap-error xmlns="urn:ietf:params:xml:ns:xcap-error"><schema-validation-error phrase=', r.body)
        r = self._put("g-0a1b2c3d", "<group", headers=self.XE)
        self.assertEqual(r.status, 409)
        self.assertIn("<not-well-formed", r.body)
        import xml.dom.minidom
        xml.dom.minidom.parseString(r.body.encode())
        r = self._put("g-0a1b2c3d", '{"name": "x"}', headers={"content-type": "application/json"})
        self.assertEqual(r.status, 415, "MIME 이 그룹 문서가 아니다(RFC 4825 §8.2.2)")
        ok = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []), headers={"content-type": "application/vnd.oma.poc.groups+xml"})
        self.assertEqual(ok.status, 200)

    # GMS-10 — CIMS 자체 요소는 cims: 이름공간에서도 읽는다
    def test_own_elements_read_from_cims_namespace_too(self):
        cims = ('<cims:on-network-require-affiliation xmlns:cims="urn:cims:groupinfo:1.0">false</cims:on-network-require-affiliation>'
                '<cims:on-network-encryption xmlns:cims="urn:cims:groupinfo:1.0">true</cims:on-network-encryption>'
                '<cims:org-code xmlns:cims="urn:cims:groupinfo:1.0">HQ</cims:org-code>')
        d = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "n", [], extra=cims))
        self.assertEqual((d["require_affiliation"], d["encryption"], d["org_code"]), (False, True, "HQ"))
        old = ('<mcpttgi:on-network-require-affiliation>true</mcpttgi:on-network-require-affiliation>'
               '<mcpttgi:org-code>OLD</mcpttgi:org-code>')
        d = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "n", [], extra=old))
        self.assertEqual((d["require_affiliation"], d["org_code"]), (True, "OLD"))

    # GMS-2 — 그룹 생성 XUI 의 tree 에 PUT: 그룹 ID 는 본문 uri, 맞지 않으면 409 uniqueness-failure + alt-value
    def test_creation_xui_assigns_group_id_via_alt_value(self):
        import re
        self.assertEqual(self._create("").status, 403, "생성 자격이 없다")
        self._grant_create()
        r = self._create("")                                  # GMS 가 정해 주길 기다린다(§6.3.2.2.2 NOTE)
        self.assertEqual((r.status, r.media_type), (409, "application/xcap-error+xml"))
        alt = re.search(r"<uniqueness-failure><exists field=\"group/list-service/@uri\"><alt-value>(tel:g-[0-9a-f]{8})</alt-value>", r.body)
        self.assertIsNotNone(alt, r.body)
        self.assertEqual(self._create("tel:g001").status, 409, "정책에 맞지 않는 ID")
        self._existing("g-0a1b2c3e", OTHER_UID)
        self.assertEqual(self._create("tel:g-0a1b2c3e").status, 409, "이미 쓰이는 ID")
        r = self._create(alt.group(1), name="새 그룹")
        self.assertEqual(r.status, 201, r.body)
        g = m.GROUPS[alt.group(1)]
        self.assertEqual((g["display_name"], g["authorized_user_id"]), ("새 그룹", OWNER_UID))
        self.assertEqual(self._create(alt.group(1)).status, 409, "같은 ID 로 다시 만들 수 없다")
        r = self._create("tel:g-0a1b2c3f", body="<group")
        self.assertEqual((r.status, r.media_type), (409, "application/xcap-error+xml"))
        self.assertEqual(self._create("tel:g-0a1b2c3f", headers={"content-type": "text/plain"}).status, 415)


class NodeSelectorTests(_Base):
    """GMS-5 — 요소·속성 단위 XCAP(TS 24.481 §6.3.6~§6.3.12 → RFC 4825 §6·§7·§8): `<문서 URI>/~~/<node selector>`."""
    GID = "g-0a1b2c3d"
    NS_Q = {"xmlns(g": "urn:3gpp:ns:mcpttGroupInfo:1.0)xmlns(r=urn:ietf:params:xml:ns:resource-lists)"}

    def setUp(self):
        super().setUp()
        self._existing(self.GID, OWNER_UID, members=["+82500000001", "+82500000002"])
        m.GROUPS[f"tel:{self.GID}"].update({"priority": 5, "group_type": "prearranged"})

    def _node(self, method, selector, body=None, xui=OWNER_PTT, headers=None, query=None, token=None):
        if token is not None:
            m.extract_token = lambda hdr: token if hdr else None
        h = {"authorization": "Bearer x"}
        h.update(headers or {})
        path = f"/org.openmobilealliance.groups/users/tel:{xui}/tel:{self.GID}/~~/{selector}"
        return _run(m.handle_group_management(
            HandlerArgs(method, path, "127.0.0.1", 0, headers=h, query_params=query or {},
                        body=body.encode() if isinstance(body, str) else body), {}))

    def _members(self):
        return [x["uri"] for x in m.GROUPS[f"tel:{self.GID}"]["members"]]

    def test_get_element_attribute_and_namespaces(self):
        r = self._node("GET", "group/list-service/display-name")
        self.assertEqual((r.status, r.media_type), (200, "application/xcap-el+xml"))
        self.assertIn(f">{self.GID}</", r.body)
        r = self._node("GET", 'group/list-service/list/entry[@uri="tel:+82500000002"]')
        self.assertEqual(r.status, 200)
        self.assertIn('uri="tel:+82500000002"', r.body)
        self.assertEqual(self._node("GET", "group/list-service/list/entry[2]/@uri").body, "tel:+82500000002")
        self.assertEqual(self._node("GET", "group/list-service/list/entry[2]/@uri").media_type, "application/xcap-att+xml")
        r = self._node("GET", "group/list-service/g:on-network-invite-members", query=self.NS_Q)
        self.assertEqual(r.status, 200, r.body)
        self.assertIn(">true<", r.body)
        r = self._node("GET", "group/list-service/namespace::*")
        self.assertEqual((r.status, r.media_type), (200, "application/xcap-ns+xml"))
        self.assertIn('xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"', r.body)
        self.assertEqual(self._node("GET", 'group/list-service/list/entry[@uri="tel:+82500000009"]').status, 404)
        self.assertEqual(self._node("GET", "group/list-service/list/entry[9]/@uri").status, 404)
        self.assertEqual(self._node("GET", "group/list-service/x:y").status, 400, "묶이지 않은 접두")
        self.assertEqual(self._node("GET", "group/list-service/list/entry[").status, 400)
        self.assertEqual(self._node("GET", "group/list-service/list/entry").status, 400, "둘 이상을 고르는 selector")

    def test_add_and_remove_one_member(self):
        sel = 'group/list-service/list/entry[@uri="tel:+82500000003"]'
        entry = ('<entry xmlns="urn:oma:xml:poc:list-service" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0" uri="tel:+82500000003">'
                 '<mcpttgi:participant-type>participant</mcpttgi:participant-type><mcpttgi:user-priority>3</mcpttgi:user-priority></entry>')
        r = self._node("PUT", sel, entry, headers={"content-type": "application/xcap-el+xml"})
        self.assertEqual(r.status, 201, r.body)
        self.assertIn("Etag", r.headers)
        self.assertEqual(self._members(), ["tel:+82500000001", "tel:+82500000002", "tel:+82500000003"])
        r = self._node("PUT", sel, entry.replace(">3<", ">7<"))          # 같은 요소를 다시 = 교체
        self.assertEqual(r.status, 200)
        self.assertEqual(m.GROUPS[f"tel:{self.GID}"]["members"][2]["priority"], 7)
        r = self._node("DELETE", 'group/list-service/list/entry[@uri="tel:+82500000001"]')
        self.assertEqual(r.status, 200)
        self.assertEqual(self._members(), ["tel:+82500000002", "tel:+82500000003"])
        self.assertEqual(self._node("DELETE", 'group/list-service/list/entry[@uri="tel:+82500000001"]').status, 404)

    def test_put_constraints(self):
        sel = 'group/list-service/list/entry[@uri="tel:+82500000003"]'
        other = '<entry xmlns="urn:oma:xml:poc:list-service" uri="tel:+82500000004"/>'
        r = self._node("PUT", sel, other)                                # 넣은 요소를 selector 가 고르지 못한다
        self.assertEqual((r.status, r.media_type), (409, "application/xcap-error+xml"))
        self.assertIn("<cannot-insert", r.body)
        r = self._node("PUT", 'group/list-service/nope/entry[@uri="tel:+82500000003"]', other)
        self.assertIn("<no-parent", r.body)
        r = self._node("PUT", sel, "<entry")
        self.assertIn("<not-xml-frag", r.body)
        bad = '<entry xmlns="urn:oma:xml:poc:list-service" xmlns:g="urn:3gpp:ns:mcpttGroupInfo:1.0" uri="tel:+82500000003"><g:participant-type>boss</g:participant-type></entry>'
        r = self._node("PUT", sel, bad)
        self.assertEqual(r.status, 409)
        self.assertIn("schema-validation-error", r.body)
        self.assertEqual(len(self._members()), 2, "거절한 쓰기는 반영하지 않는다")
        r = self._node("PUT", "group/list-service/@uri", "tel:g-ffffffff")
        self.assertIn("constraint-failure", r.body)
        r = self._node("DELETE", "group/list-service/g:on-network-invite-members", query=self.NS_Q)
        self.assertEqual(r.status, 409)
        self.assertIn("<cannot-delete", r.body)

    def test_replace_display_name_and_if_match(self):
        new = '<display-name xmlns="urn:oma:xml:poc:list-service">작전 1팀</display-name>'
        etag = self._node("GET", "group/list-service/display-name").headers["Etag"]
        self.assertEqual(self._node("PUT", "group/list-service/display-name", new, headers={"if-match": '"stale"'}).status, 412)
        r = self._node("PUT", "group/list-service/display-name", new, headers={"if-match": etag})
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(m.GROUPS[f"tel:{self.GID}"]["display_name"], "작전 1팀")

    def test_authorisation_follows_document_rules(self):
        other = {"sub": OTHER_LOGIN, "mcptt_id": f"tel:{OTHER_PTT}", "scope": m.SCOPE_PTT_GMS}
        r = self._node("GET", "group/list-service/display-name", xui=OTHER_PTT, token=other)
        self.assertEqual(r.status, 403, "비멤버는 읽지 못한다")
        m.GROUPS[f"tel:{self.GID}"]["members"].append({"uri": f"tel:{OTHER_PTT}", "name": "o", "role": "participant", "priority": 0})
        self.assertEqual(self._node("GET", "group/list-service/display-name", xui=OTHER_PTT, token=other).status, 200)
        r = self._node("DELETE", 'group/list-service/list/entry[@uri="tel:+82500000001"]', xui=OTHER_PTT, token=other)
        self.assertEqual(r.status, 403, "멤버라도 소유자가 아니면 쓰지 못한다")
        self.assertEqual(self._node("POST", "group/list-service/display-name", "<x/>", token=self.token).status, 405)


class DbWriteTests(unittest.TestCase):
    def setUp(self):
        self._saved = (m._db_connect, m.sync_group_from_db)
        m.sync_group_from_db = lambda gid: True

    def tearDown(self):
        m._db_connect, m.sync_group_from_db = self._saved

    def test_required_members_cannot_exceed_max(self):
        # TS 24.379 §6.3.5.5 NOTE 4 — 정원 < 필수 멤버 수는 GMS 가 거절한다(관리 API 와 같은 규칙, XCAP 경로도)
        cur = _FakeCursor({"+82500000001", "+82500000002"})
        m._db_connect = lambda: _FakeConn(cur)
        req2 = _doc("tel:g-0a1b2c3d", "n", [("tel:+82500000001", "chair", 1), ("tel:+82500000002", "participant", 0)],
                    extra="<mcpttgi:on-network-max-participant-count>1</mcpttgi:on-network-max-participant-count>")
        req2 = req2.replace("<mcpttgi:participant-type>", "<mcpttgi:on-network-required/><mcpttgi:participant-type>")
        st, err = m.gms_write_group("g-0a1b2c3d", m.parse_group_document_xml(req2), 5020, create=True)
        self.assertEqual((st, err.get("error")), (400, "required_exceeds_max_members"))
        self.assertFalse(any(q.startswith("INSERT INTO ptt_groups") for q, _ in cur.sql))
        # 갱신 — 문서가 정원만 줄이면 DB 의 필수 멤버 수로 본다
        cur = _FakeCursor(set(), existing_pk=9)
        cur.group_row = {"max_members": 0, "n_req": 3}
        m._db_connect = lambda: _FakeConn(cur)
        only_max = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">'
            '<list-service uri="tel:g-0a1b2c3d"><mcpttgi:on-network-max-participant-count>2</mcpttgi:on-network-max-participant-count>'
            '</list-service></group>')
        st, err = m.gms_write_group("g-0a1b2c3d", only_max, 5020, create=False)
        self.assertEqual(st, 400)
        cur.group_row = {"max_members": 0, "n_req": 2}
        self.assertEqual(m.gms_write_group("g-0a1b2c3d", only_max, 5020, create=False), (0, {}))

    def test_create_inserts_defaults_owner_and_members(self):
        cur = _FakeCursor({"+82500000001", "+82500000002"})
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "관제채널",
                                              [("tel:+82500000001", "chair", 1), ("tel:+82500000002", "participant", 0)], "chat"))
        st, err = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=True)
        self.assertEqual((st, err), (0, {}))
        ins = next(q for q, a in cur.sql if q.startswith("INSERT INTO ptt_groups"))
        args = next(a for q, a in cur.sql if q.startswith("INSERT INTO ptt_groups"))
        self.assertIn("authorized_user_id", ins)
        self.assertEqual(args[0], "g-0a1b2c3d"); self.assertEqual(args[1], "관제채널"); self.assertEqual(args[-1], 5020)
        self.assertEqual(args[2 + m._GMS_ATTR_COLS.index("group_type")], "chat")
        self.assertEqual(args[2 + m._GMS_ATTR_COLS.index("priority")], 5)            # 기본값
        self.assertEqual(args[2 + m._GMS_ATTR_COLS.index("allow_conference_state")], 1)   # 기본 허용
        self.assertIn("allow_conference_state", ins)
        mem = [a for q, a in cur.sql if q.startswith("INSERT IGNORE INTO ptt_group_members")]
        self.assertEqual([(a[1], a[2], a[3]) for a in mem], [("+82500000001", 1, "chair"), ("+82500000002", 0, "participant")])
        self.assertTrue(any(q.startswith("DELETE FROM ptt_group_members") for q, _ in cur.sql))

    def test_unknown_member_400_and_no_insert(self):
        cur = _FakeCursor({"+82500000001"})
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "n", [("tel:+82599999999", "participant", 0)]))
        st, err = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=True)
        self.assertEqual(st, 400); self.assertEqual(err["error"], "unknown_member"); self.assertEqual(err["detail"], ["+82599999999"])
        self.assertFalse(any(q.startswith("INSERT INTO ptt_groups") for q, _ in cur.sql))

    def test_update_only_given_fields_keeps_members_when_absent(self):
        cur = _FakeCursor(set(), existing_pk=42)
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">'
            '<list-service uri="tel:g-0a1b2c3d"><display-name>새이름</display-name>'
            '<mcpttgi:on-network-group-priority>2</mcpttgi:on-network-group-priority></list-service></group>')
        st, _ = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=False)
        self.assertEqual(st, 0)
        upd = next((q, a) for q, a in cur.sql if q.startswith("UPDATE ptt_groups"))
        self.assertEqual(upd[0], "UPDATE ptt_groups SET name=%s, priority=%s WHERE id=%s")
        self.assertEqual(upd[1], ("새이름", 2, 42))
        self.assertFalse(any(q.startswith("DELETE FROM ptt_group_members") for q, _ in cur.sql))

    def test_update_roundtrip_of_zero_timers_keeps_db(self):
        """T4 0·TNG3 0 인 편성 그룹의 GET 문서를 그대로 PUT — T4 는 요소가 없어 열을 건드리지 않고, TNG3 무제한 표기는 0 으로
        써서 DB 값이 바뀌지 않는다(mcptt_timers.md §7 D8)."""
        m.GROUPS["tel:g-0a1b2c3d"] = {"display_name": "n", "etag": "e", "group_type": "prearranged",
                                      "hang_timer_sec": 0, "max_duration_sec": 0, "members": []}
        try:
            xml, _ = m.get_group_xml("tel:g-0a1b2c3d")
        finally:
            m.GROUPS.pop("tel:g-0a1b2c3d", None)
        cur = _FakeCursor(set(), existing_pk=42)
        m._db_connect = lambda: _FakeConn(cur)
        st, _ = m.gms_write_group("g-0a1b2c3d", m.parse_group_document_xml(xml), 5020, create=False)
        self.assertEqual(st, 0)
        q, a = next((q, a) for q, a in cur.sql if q.startswith("UPDATE ptt_groups"))
        self.assertNotIn("hang_timer_sec", q)
        cols = [c.split("=")[0].strip() for c in q[len("UPDATE ptt_groups SET "):q.index(" WHERE")].split(",")]
        self.assertEqual(a[cols.index("max_duration_sec")], 0)

    def test_update_missing_group_404_and_delete(self):
        cur = _FakeCursor(set(), existing_pk=None)
        m._db_connect = lambda: _FakeConn(cur)
        st, err = m.gms_write_group("g-0a1b2c3d", {"display_name": "x", "members": None}, 5020, create=False)
        self.assertEqual((st, err["error"]), (404, "not_found"))
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d")[0], 404)
        cur2 = _FakeCursor(set(), existing_pk=42)
        m._db_connect = lambda: _FakeConn(cur2)
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d"), (0, {}))

    def test_no_db_config_503(self):
        m._db_connect = lambda: None
        self.assertEqual(m.gms_write_group("g-0a1b2c3d", {}, 1, True)[0], 503)
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d")[0], 503)


if __name__ == "__main__":
    unittest.main()
