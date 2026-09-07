"""csc/src/services/mcptt.py — MCPTT user-profile 문서(TS 24.484 §8.3.2) 단위 시험 (오프라인, DB 없음).

규격 단말은 이 문서에서 그룹 목록(<OnNetwork><MCPTTGroupInfo>)·연락처(<PrivateCallList>)·긴급 대상·인가를 읽는다.
검사: XSD 요소 집합/순서(OnNetworkType 에 MCPTTUserID 없음, EntryType 은 uri-entry 필수), 그룹 목록 = 소속 그룹
(소유만 한 그룹 제외, 소유 소속 그룹은 cims:authorized-user), ImplicitAffiliations = 소속 전체, 연락처 = 동료 멤버,
긴급그룹 미지정(DedicatedGroup) = Emergency 요소 부재(빈 entry 금지), UseCurrentlySelectedGroup 폴백 uri-entry,
common-policy ruleset, escape, ETag 내용 파생, 단말 정규식 호환(첫 MCPTTGroupInitiation = EmergencyCall).

  python3 -m unittest tests.test_csc_user_profile
"""
from __future__ import annotations

import os
import re
import sys
import unittest
import xml.etree.ElementTree as ET

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

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
        self.assertEqual(impl, uris, "암묵 제휴 = 소속 전체")
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

    def test_emergency_unconfigured_omits_elements(self):
        _, root, _ = self._doc()   # PTT_PROFILES 부재 = DedicatedGroup + 긴급그룹 미지정
        gc = root.find("up:Common/up:MCPTT-group-call", NS)
        self.assertIsNone(gc.find("up:EmergencyCall", NS))
        self.assertIsNone(gc.find("up:ImminentPerilCall", NS))
        self.assertIsNone(gc.find("up:EmergencyAlert", NS))
        self.assertIsNone(root.find("up:OnNetwork/up:PrivateEmergencyAlert", NS))
        self.assertIsNone(root.find("up:Common/up:PrivateCall/up:EmergencyCall", NS))

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
        self.assertEqual(acts.find("cims:allow-adhoc-group-call", NS).text, "true")
        # 단말 정규식(태그명만) 호환
        self.assertRegex(xml, r"<allow-emergency-group-call>\s*false\s*</allow-emergency-group-call>")

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


if __name__ == "__main__":
    unittest.main()
