"""csc/src/services/mcptt.py — MCPTT user-profile 문서(TS 24.484 §8.3.2) 단위 시험 (오프라인, DB 없음).

규격 단말은 이 문서에서 그룹 목록(<OnNetwork><MCPTTGroupInfo>)·연락처(<PrivateCallList>)·긴급 대상·인가를 읽는다.
검사: XSD 요소 집합/순서(OnNetworkType 에 MCPTTUserID 없음, EntryType 은 uri-entry 필수), 그룹 목록 = 소속 그룹
(소유만 한 그룹 제외, 소유 소속 그룹은 cims:authorized-user), ImplicitAffiliations = 내 멤버 행의 implicit_affiliation 이
켜진 그룹만(남의 설정은 안 따름, 없으면 요소 생략), 연락처 = 동료 멤버,
긴급 요소는 항상 존재(§8.3.2.1 shall) — 미지정은 entry-info 폴백 + ruleset 미인가, ProSe User-Info-ID 영값, 루트 Status,
선택이지만 필수로 읽는 단말용으로 항상 싣는 것(alias-entry index·xml:lang, ParticipantType — xml:lang 은 Name 과 같은 값),
common-policy ruleset, escape, ETag 내용 파생, 단말 정규식 호환(첫 MCPTTGroupInitiation = EmergencyCall),
ruleset anyExt 의 미응답 멤버 알림 자격(allow-to-receive-non-acknowledged-users-information, TS 24.379 §6.3.3.3) —
admin API 프로파일 GET/PUT 의 선택 컬럼 규약(부재 = false·입력 400)과 캐시 반영까지.

  python3 -m unittest tests.test_csc_user_profile
"""
from __future__ import annotations

import asyncio
import os
import re
import sys
import unittest
import xml.etree.ElementTree as ET

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

NS = {"up": "urn:3gpp:mcptt:user-profile:1.0", "cp": "urn:ietf:params:xml:ns:common-policy",
      "cims": "urn:cims:mcptt:ext:1.0"}
ME, PEER, PEER2, OUTSIDER = "tel:+82500000001", "tel:+82500000002", "tel:+82500000003", "tel:+82500000009"


def _grp(name, members, owner_uid=None):
    return {"display_name": name, "etag": "e", "authorized_user_id": owner_uid,
            "members": [{"uri": u, "name": n, "role": "participant", "priority": 5} for u, n in members]}


class UserProfileDocTest(unittest.TestCase):
    def setUp(self):
        self._keep = (dict(m.USERS), dict(m.GROUPS), dict(m.PTT_PROFILES), dict(m.SERVICE_CONFIG), dict(m.USER_PROFILE_CONFIG))
        m.USERS.clear(); m.GROUPS.clear(); m.PTT_PROFILES.clear(); m.USER_PROFILE_CONFIG.clear()
        m.USERS[ME] = {"msisdn": "+82500000001", "name": "홍길동 & 팀", "password": ""}
        m.GROUPS["tel:g001"] = _grp("음성그룹1", [(ME, "홍길동"), (PEER, "김철수"), (PEER2, "박영희")], owner_uid=77)
        m.GROUPS["tel:g002"] = _grp("음성그룹2", [(ME, "홍길동"), (PEER, "김철수")])
        m.GROUPS["tel:g-owned"] = _grp("내가만든그룹", [(OUTSIDER, "외부인")], owner_uid=77)   # 소유만, 비멤버
        m.GROUPS["tel:g003"] = _grp("남의그룹", [(OUTSIDER, "외부인")])
        # 암시적 제휴 — g002 는 나(ME) 에게, g001 은 동료(PEER) 에게만 켜 둔다(내 문서엔 g002 만 나와야 한다)
        m.GROUPS["tel:g002"]["members"][0]["implicit_affiliation"] = True
        m.GROUPS["tel:g001"]["members"][1]["implicit_affiliation"] = True
        m.SERVICE_CONFIG["max_affiliations_n2"] = 12

    def tearDown(self):
        users, groups, profs, sc, upc = self._keep
        m.USERS.clear(); m.USERS.update(users); m.GROUPS.clear(); m.GROUPS.update(groups)
        m.PTT_PROFILES.clear(); m.PTT_PROFILES.update(profs); m.SERVICE_CONFIG.clear(); m.SERVICE_CONFIG.update(sc)
        m.USER_PROFILE_CONFIG.clear(); m.USER_PROFILE_CONFIG.update(upc)

    def _doc(self, owner_uid=None):
        xml, etag = m.get_user_profile_xml(ME, owner_uid=owner_uid)
        self.assertTrue(xml and etag)
        return xml, ET.fromstring(xml.encode()), etag

    def test_root_and_common_structure(self):
        xml, root, _ = self._doc()
        self.assertEqual(root.tag, "{%s}mcptt-user-profile" % NS["up"])
        self.assertEqual(root.get("XUI-URI"), ME)
        self.assertEqual(root.get("user-profile-index"), "1")
        self.assertEqual(root.find("up:Name", NS).text, "홍길동 & 팀", "Name 은 NameType 단순 내용 + escape")
        common = root.find("up:Common", NS)
        self.assertEqual(common.get("index"), "1")
        self.assertEqual(common.find("up:UserAlias/up:alias-entry", NS).text, "홍길동 & 팀")
        self.assertEqual(common.find("up:MCPTTUserID/up:uri-entry", NS).text, ME, "MCPTTUserID 는 EntryType(uri-entry)")
        self.assertEqual(common.find("up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "1")
        self.assertEqual(common.find("up:MCPTT-group-call/up:Priority", NS).text, "0")
        self.assertEqual(common.find("up:MissionCriticalOrganization", NS).text, m._UE_INIT_DEFAULTS["Name"])
        self.assertIsNone(common.find("up:PrivateCall/up:MaxCallsN7", NS), "MaxCallsN7 은 XSD 에 없음")

    def test_onnetwork_group_list(self):
        _, root, _ = self._doc(owner_uid=77)
        on = root.find("up:OnNetwork", NS)
        self.assertEqual(on.get("index"), "1")
        self.assertIsNone(on.find("up:MCPTTUserID", NS), "OnNetworkType 에 MCPTTUserID 없음")
        uris = [e.find("up:uri-entry", NS).text for e in on.findall("up:MCPTTGroupInfo/up:entry", NS)]
        self.assertEqual(uris, ["tel:g001", "tel:g002"], "소속 그룹만(소유-비멤버·남의 그룹 제외), URI 순")
        names = [e.find("up:display-name", NS).text for e in on.findall("up:MCPTTGroupInfo/up:entry", NS)]
        self.assertEqual(names, ["음성그룹1", "음성그룹2"])
        owned = on.find("up:MCPTTGroupInfo/up:entry[up:uri-entry='tel:g001']/up:anyExt/cims:authorized-user", NS)
        self.assertEqual(owned.text, "true", "소유한 소속 그룹은 anyExt 표시")
        self.assertIsNone(on.find("up:MCPTTGroupInfo/up:entry[up:uri-entry='tel:g002']/up:anyExt", NS))
        self.assertEqual(on.find("up:MaxAffiliationsN2", NS).text, "12", "service config max_affiliations_n2")
        impl = [e.find("up:uri-entry", NS).text for e in on.findall("up:ImplicitAffiliations/up:entry", NS)]
        self.assertEqual(impl, ["tel:g002"], "암시적 제휴 = 내 멤버 행에 켜진 그룹만(동료 설정은 안 따름)")
        self.assertEqual(on.find("up:MaxSimultaneousTransmissionsN7", NS).text, "1")
        for e in root.iter("{%s}entry" % NS["up"]):
            self.assertIsNotNone(e.find("up:uri-entry", NS), "EntryType 은 uri-entry 필수 — 빈 entry 금지")

    def test_no_owner_marker_without_owner_uid(self):
        _, root, _ = self._doc(owner_uid=None)
        self.assertEqual(root.findall(".//cims:authorized-user", NS), [])

    def test_contacts_are_group_peers(self):
        _, root, _ = self._doc()
        uris = sorted(e.find("up:uri-entry", NS).text for e in root.findall("up:Common/up:PrivateCall/up:PrivateCallList/up:PrivateCallURI", NS))
        self.assertEqual(uris, sorted([PEER, PEER2]), "동료 멤버(본인 제외, 중복 제거), 외부인 제외")

    def test_emergency_unconfigured_elements_present_with_fallback(self):
        """§8.3.2.1 8d)ii·8e)ii~iv·10f) 필수 요소는 대상 미지정에도 항상 존재 — entry-info 폴백 + ruleset 미인가."""
        _, root, _ = self._doc()   # PTT_PROFILES 부재 = DedicatedGroup+긴급그룹 미지정, LocallyDetermined+수신자 미지정
        gc = root.find("up:Common/up:MCPTT-group-call", NS)
        for tag in ("EmergencyCall/up:MCPTTGroupInitiation", "ImminentPerilCall/up:MCPTTGroupInitiation", "EmergencyAlert"):
            e = gc.find(f"up:{tag}/up:entry", NS)
            self.assertIsNotNone(e, tag)
            self.assertEqual((e.get("entry-info"), e.find("up:uri-entry", NS).text), ("UseCurrentlySelectedGroup", "tel:g001"),
                             f"{tag}: 미지정 → 선택 그룹 사용 + 폴백 = 첫 소속 그룹")
        pr = root.find("up:Common/up:PrivateCall/up:EmergencyCall/up:MCPTTPrivateRecipient", NS)
        self.assertIsNotNone(pr)
        e = pr.find("up:entry", NS)
        self.assertEqual((e.get("entry-info"), e.find("up:uri-entry", NS).text), ("LocallyDetermined", PEER),
                         "미지정 → 발신자 선택 + 폴백 = 첫 연락처")
        self.assertEqual(pr.find("up:ProSeUserID-entry/up:User-Info-ID", NS).text, "000000000000", "ProSe 필수 자식 = 6옥텟 영값")
        pea = root.find("up:OnNetwork/up:PrivateEmergencyAlert/up:entry", NS)
        self.assertEqual((pea.get("entry-info"), pea.find("up:uri-entry", NS).text), ("LocallyDetermined", PEER))
        # 인가는 요소 유무가 아니라 ruleset — 그룹 대상 미지정이면 false(CSP 403 과 일치), 사설은 LocallyDetermined 라 true
        acts = root.find("cp:ruleset/cp:rule/cp:actions", NS)
        self.assertEqual(acts.find("up:allow-emergency-group-call", NS).text, "false")
        self.assertEqual(acts.find("up:allow-activate-emergency-alert", NS).text, "false")
        self.assertEqual(acts.find("up:allow-emergency-private-call", NS).text, "true")

    def test_root_status_and_always_present_optionals(self):
        """규격상 선택이지만 필수로 읽는 단말이 있어 항상 싣는 것 — alias-entry index·xml:lang, ParticipantType."""
        XL = "{http://www.w3.org/XML/1998/namespace}lang"
        _, root, _ = self._doc()
        self.assertEqual(root.find("up:Status", NS).text, "true", "§8.3.2.1 3) shall include one <Status>")
        al = root.find("up:Common/up:UserAlias/up:alias-entry", NS)
        self.assertEqual(al.get("index"), "1")
        self.assertEqual(al.get(XL), "en", "alias-entry xml:lang = UserProfile.Language 기본값")
        self.assertEqual(root.find("up:Name", NS).get(XL), al.get(XL), "Name 과 alias-entry 의 xml:lang 은 같은 값")
        self.assertEqual(root.find("up:Common/up:ParticipantType", NS).text, "user", "§8.3.2.1 f) 기본값")

    def test_participant_type_and_language_configurable(self):
        m.USER_PROFILE_CONFIG.update({"ParticipantType": "dispatch", "Language": "ko"})
        XL = "{http://www.w3.org/XML/1998/namespace}lang"
        _, root, _ = self._doc()
        self.assertEqual(root.find("up:Common/up:ParticipantType", NS).text, "dispatch")
        self.assertEqual(root.find("up:Common/up:UserAlias/up:alias-entry", NS).get(XL), "ko")
        self.assertEqual(root.find("up:Name", NS).get(XL), "ko")

    def test_emergency_dedicated_group_configured(self):
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, emergency_group_mode="DedicatedGroup",
                                             emergency_group_id="g002", private_emergency_mode="UsePreConfigured",
                                             emergency_private_recipient="+82500000002")
        xml, root, _ = self._doc()
        e = root.find("up:Common/up:MCPTT-group-call/up:EmergencyCall/up:MCPTTGroupInitiation/up:entry", NS)
        self.assertEqual((e.get("entry-info"), e.find("up:uri-entry", NS).text, e.find("up:display-name", NS).text),
                         ("DedicatedGroup", "tel:g002", "음성그룹2"))
        self.assertIsNotNone(root.find("up:Common/up:MCPTT-group-call/up:ImminentPerilCall/up:MCPTTGroupInitiation/up:entry", NS))
        self.assertEqual(root.find("up:Common/up:MCPTT-group-call/up:EmergencyAlert/up:entry/up:uri-entry", NS).text, "tel:g002")
        pr = root.find("up:Common/up:PrivateCall/up:EmergencyCall/up:MCPTTPrivateRecipient/up:entry", NS)
        self.assertEqual((pr.get("entry-info"), pr.find("up:uri-entry", NS).text), ("UsePreConfigured", "tel:+82500000002"))
        self.assertEqual(root.find("up:OnNetwork/up:PrivateEmergencyAlert/up:entry/up:uri-entry", NS).text, "tel:+82500000002")
        self.assertEqual(root.find("up:Common/up:PrivateCall/up:EmergencyCall/up:MCPTTPrivateRecipient/up:ProSeUserID-entry/up:User-Info-ID", NS).text,
                         "000000000000")
        acts = root.find("cp:ruleset/cp:rule/cp:actions", NS)
        self.assertEqual(acts.find("up:allow-emergency-group-call", NS).text, "true", "전용 그룹 지정 → 인가 유지")
        # 단말 정규식 호환: 첫 <MCPTTGroupInitiation> 블록이 EmergencyCall 의 것
        gi = re.search(r"<MCPTTGroupInitiation>(.*?)</MCPTTGroupInitiation>", xml, re.S).group(1)
        self.assertIn('entry-info="DedicatedGroup"', gi); self.assertIn("<uri-entry>tel:g002</uri-entry>", gi)
        self.assertLess(xml.index("<EmergencyCall>"), xml.index("<ImminentPerilCall>"))

    def test_emergency_use_selected_group_fallback_uri(self):
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, emergency_group_mode="UseCurrentlySelectedGroup",
                                             emergency_group_id=None)
        _, root, _ = self._doc()
        e = root.find("up:Common/up:MCPTT-group-call/up:EmergencyCall/up:MCPTTGroupInitiation/up:entry", NS)
        self.assertEqual((e.get("entry-info"), e.find("up:uri-entry", NS).text), ("UseCurrentlySelectedGroup", "tel:g001"),
                         "미선택 시 폴백 = 첫 소속 그룹 (uri-entry 필수)")

    def test_ruleset_common_policy_and_flags(self):
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, allow_emergency_call=False, allow_ambient_listening=True)
        xml, root, _ = self._doc()
        acts = root.find("cp:ruleset/cp:rule/cp:actions", NS)
        self.assertIsNotNone(acts, "ruleset 은 RFC 4745 common-policy 네임스페이스")
        self.assertEqual(acts.find("up:allow-emergency-group-call", NS).text, "false")
        self.assertEqual(acts.find("up:allow-ambient-listening", NS).text, "true")
        # ad hoc 인가 = 규격 anyExt 자식(TS 24.484 §8.3.2.1 11)xxxviii)R)) + 전환기 별칭
        self.assertEqual(acts.find("up:anyExt/up:allow-adhoc-group-call", NS).text, "true")
        self.assertEqual(acts.find("cims:allow-adhoc-group-call", NS).text, "true")
        # 단말 정규식(태그명만) 호환
        self.assertRegex(xml, r"<allow-emergency-group-call>\s*false\s*</allow-emergency-group-call>")

    def test_non_ack_users_info_in_anyext(self):
        # TS 24.484 §8.3.2.1 11)xxxviii)L) / 표 8.3.2.7-49 — 부재 = false 가 규격 기본값, 프로파일 기본도 false
        _, root, _ = self._doc()
        ext = root.find("cp:ruleset/cp:rule/cp:actions/up:anyExt", NS)
        self.assertEqual(ext.find("up:allow-to-receive-non-acknowledged-users-information", NS).text, "false")
        self.assertEqual([c.tag.split("}")[1] for c in ext],
                         ["allow-to-receive-non-acknowledged-users-information", "allow-adhoc-group-call"],
                         "anyExt 자식 순서 = §8.3.2.1 11)xxxviii) 목록 순(L → R)")
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, allow_non_ack_users_info=True)
        _, root, _ = self._doc()
        self.assertEqual(root.find("cp:ruleset/cp:rule/cp:actions/up:anyExt/"
                                   "up:allow-to-receive-non-acknowledged-users-information", NS).text, "true")
        # 옛 캐시 항목(키 없음)도 false
        m.PTT_PROFILES["+82500000001"] = {k: v for k, v in m.DEFAULT_USER_PROFILE.items() if k != "allow_non_ack_users_info"}
        _, root, _ = self._doc()
        self.assertEqual(root.find(".//up:allow-to-receive-non-acknowledged-users-information", NS).text, "false")

    def test_user_profile_config_and_etag(self):
        _, root, etag1 = self._doc()
        m.USER_PROFILE_CONFIG.update({"MaxSimultaneousCallsN6": 3, "MissionCriticalOrganization": "포인티 <PS>"})
        xml, root2, etag2 = self._doc()
        self.assertEqual(root2.find("up:Common/up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "3")
        self.assertEqual(root2.find("up:Common/up:MissionCriticalOrganization", NS).text, "포인티 <PS>")
        self.assertNotEqual(etag1, etag2, "ETag 는 내용 파생")
        m.GROUPS["tel:g002"]["members"].append({"uri": OUTSIDER, "name": "외부인"})
        _, _, etag3 = self._doc()
        self.assertNotEqual(etag2, etag3, "멤버십 변화도 ETag 변화(연락처)")

    def test_unknown_user(self):
        self.assertEqual(m.get_user_profile_xml("tel:+0"), (None, None))


class _ProfCur:
    """admin 프로파일 GET/PUT 이 내는 SQL 만 흉내 내는 DictCursor — cols = 선택 컬럼 중 DB 에 있는 것."""

    def __init__(self, cols, row=None):
        self.cols = set(cols)
        self.row = row
        self.inserted = None
        self._rows = []

    def execute(self, sql, params=None):
        s = " ".join(sql.split())
        self._rows = []
        if s.startswith("SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s"):
            self._rows = [{"1": 1}]
        elif s.startswith("SHOW COLUMNS FROM ptt_user_profile LIKE "):
            col = s.split("LIKE ")[1].strip("'")
            self._rows = [{"Field": col}] if col in self.cols else []
        elif s.startswith("SELECT allow_emergency_call") and "FROM ptt_user_profile WHERE ptt_id=%s" in s:
            if self.row is not None:
                r = dict(self.row)
                for c in ("allow_ambient_listening", "allow_create_group", "allow_non_ack_users_info"):
                    if f"0 AS {c}" in s:
                        r[c] = 0                     # 부재 컬럼 = 상수 0 별칭
                self._rows = [r]
        elif s.startswith("INSERT INTO ptt_user_profile"):
            head = s.split("(", 1)[1].split(")", 1)[0]
            self.inserted = dict(zip([c.strip() for c in head.split(",")], params))
        else:
            raise AssertionError(f"unexpected SQL: {s}")

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class _ProfConn:
    def __init__(self, cur):
        self.cur = cur

    def cursor(self):
        return self.cur

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class AdminProfileNonAckTest(unittest.TestCase):
    """admin API `/users/{pid}/ptt/{msisdn}/profile` 의 allow_non_ack_users_info — 선택 컬럼 규약
    (부재 = 응답 false·입력 400 schema_not_migrated) + 캐시 반영 → user-profile anyExt."""

    MSISDN = "+82500000001"

    @classmethod
    def setUpClass(cls):
        import handlers.admin as adm
        cls.adm = adm

    def setUp(self):
        self._keep = (dict(m.USERS), dict(m.PTT_PROFILES), self.adm._get_db, self.adm.notify_csp)
        self.adm._OPT_COL_PRESENT.clear()
        self.adm.notify_csp = lambda *a, **k: None
        m.USERS[ME] = {"msisdn": self.MSISDN, "name": "홍길동", "password": ""}

    def tearDown(self):
        users, profs, get_db, notify = self._keep
        m.USERS.clear(); m.USERS.update(users); m.PTT_PROFILES.clear(); m.PTT_PROFILES.update(profs)
        self.adm._get_db, self.adm.notify_csp = get_db, notify
        self.adm._OPT_COL_PRESENT.clear()

    def _with(self, cur):
        self.adm._get_db = lambda config: _ProfConn(cur)

    def test_put_writes_column_and_profile_doc(self):
        cur = _ProfCur({"allow_ambient_listening", "allow_create_group", "allow_non_ack_users_info"})
        self._with(cur)
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_non_ack_users_info": True}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertTrue(r.body["allow_non_ack_users_info"])
        self.assertEqual(cur.inserted.get("allow_non_ack_users_info"), 1)
        self.assertEqual(cur.inserted.get("allow_create_group"), 0, "요청에 없는 선택 자격은 0(종전 규약)")
        xml, _ = m.get_user_profile_xml(ME)
        root = ET.fromstring(xml.encode())
        self.assertEqual(root.find("cp:ruleset/cp:rule/cp:actions/up:anyExt/"
                                   "up:allow-to-receive-non-acknowledged-users-information", NS).text, "true")

    def test_put_absent_column_is_400_with_migration_hint(self):
        self._with(_ProfCur({"allow_ambient_listening", "allow_create_group"}))
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_non_ack_users_info": False}, {}))
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body["error"], "schema_not_migrated")
        self.assertIn("migrate_ptt_non_ack_users_info.sql", r.body["detail"])

    def test_put_absent_column_without_key_is_ok(self):
        cur = _ProfCur({"allow_ambient_listening", "allow_create_group"})
        self._with(cur)
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_create_group": True}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertNotIn("allow_non_ack_users_info", cur.inserted, "부재 컬럼은 INSERT 에 싣지 않는다")
        self.assertFalse(r.body["allow_non_ack_users_info"])

    def test_get_absent_column_reads_false(self):
        row = {"allow_emergency_call": 1, "allow_emergency_alert": 1, "allow_adhoc_call": 1,
               "allow_emergency_private_call": 1, "emergency_group_mode": "DedicatedGroup", "emergency_group_id": None,
               "private_emergency_mode": "LocallyDetermined", "emergency_private_recipient": None,
               "allow_ambient_listening": 0, "allow_create_group": 1, "allow_non_ack_users_info": 1}
        self._with(_ProfCur({"allow_ambient_listening", "allow_create_group"}, row=row))
        r = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertEqual(r.status, 200)
        self.assertIs(r.body["allow_non_ack_users_info"], False)
        self.assertIs(r.body["allow_create_group"], True)
        self.adm._OPT_COL_PRESENT.clear()
        self._with(_ProfCur({"allow_ambient_listening", "allow_create_group", "allow_non_ack_users_info"}, row=row))
        r = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertIs(r.body["allow_non_ack_users_info"], True)

    def test_default_profile_has_key(self):
        self.assertIs(m.DEFAULT_USER_PROFILE["allow_non_ack_users_info"], False)


SC = {"sc": "urn:3gpp:ns:mcpttServiceConfig:1.0"}


class ServiceConfigDocTest(unittest.TestCase):
    """service configuration 문서 — TS 24.484 §8.4.2.1·§8.4.2.3 스키마 구조."""

    def setUp(self):
        self._keep = (dict(m.SERVICE_CONFIG), dict(m.SERVICE_CONFIG_PARAMS))

    def tearDown(self):
        sc, params = self._keep
        m.SERVICE_CONFIG.clear(); m.SERVICE_CONFIG.update(sc)
        m.SERVICE_CONFIG_PARAMS.clear(); m.SERVICE_CONFIG_PARAMS.update(params)

    def _doc(self):
        xml, etag = m.get_service_config_xml(None)
        return ET.fromstring(xml.encode()), etag

    def test_structure(self):
        m.SERVICE_CONFIG.update({"num_levels_group_hierarchy": 4, "num_levels_user_hierarchy": 5})
        root, _ = self._doc()
        self.assertEqual(root.tag, "{%s}service-configuration-info" % SC["sc"])
        params = root.find("sc:service-configuration-params", SC)
        self.assertTrue(params.get("domain"), "domain 속성 필수(§8.4.2.1 1))")
        self.assertEqual(params.find("sc:common/sc:broadcast-group/sc:num-levels-group-hierarchy", SC).text, "4")
        self.assertEqual(params.find("sc:common/sc:broadcast-group/sc:num-levels-user-hierarchy", SC).text, "5")
        on = params.find("sc:on-network", SC)
        fc = on.find("sc:fc-timers-counters", SC)
        self.assertEqual([c.tag.split("}")[1] for c in fc][:3], ["T1-end-of-rtp-media", "T3-stop-talking-grace", "T7-floor-idle"])
        self.assertEqual(len(list(fc)), 17, "fc-timers-countersType 시퀀스 17 요소(필수)")
        self.assertEqual(fc.find("sc:T16-map-group-to-bearer", SC).text, "PT0.5S")
        self.assertEqual(on.find("sc:transmit-time/sc:time-limit", SC).text, "PT30S", "T2 = transmit-time/time-limit")
        # 스키마 시퀀스 — fc-timers-counters 뒤 RP 셋, 그 순서
        tags = [c.tag.split("}")[1] for c in on]
        self.assertEqual(tags, ["transmit-time", "fc-timers-counters", "emergency-resource-priority",
                                "imminent-peril-resource-priority", "normal-resource-priority"])
        e = on.find("sc:emergency-resource-priority", SC)
        self.assertEqual((e.find("sc:resource-priority-namespace", SC).text, e.find("sc:resource-priority-priority", SC).text),
                         ("mcpttp", "15"))
        # 인가 요소는 service-config 에 없다(§8.4)
        xml, _ = m.get_service_config_xml(None)
        for gone in ("allow-private-call", "allow-emergency-call", "allow-alert", "allow-transmit-request",
                     "allow-create-delete-group", "max-affiliations-N2"):
            self.assertNotIn(gone, xml)

    def test_params_override_and_etag(self):
        _, etag1 = self._doc()
        m.SERVICE_CONFIG_PARAMS.update({"FcTimersCounters": {"T1-end-of-rtp-media": 6000},
                                        "ResourcePriority": {"Emergency": "14"}})
        root, etag2 = self._doc()
        on = root.find("sc:service-configuration-params/sc:on-network", SC)
        self.assertEqual(on.find("sc:fc-timers-counters/sc:T1-end-of-rtp-media", SC).text, "PT6S")
        self.assertEqual(on.find("sc:fc-timers-counters/sc:T3-stop-talking-grace", SC).text, "PT3S", "미지정 = 기본값")
        self.assertEqual(on.find("sc:emergency-resource-priority/sc:resource-priority-priority", SC).text, "14")
        self.assertNotEqual(etag1, etag2, "ETag 는 내용 파생")


if __name__ == "__main__":
    unittest.main()
