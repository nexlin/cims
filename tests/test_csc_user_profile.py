"""csc/src/services/mcptt.py — MCPTT user-profile 문서(TS 24.484 §8.3.2) 단위 시험 (오프라인, DB 없음).

규격 단말은 이 문서에서 그룹 목록(<OnNetwork><MCPTTGroupInfo>)·연락처(<PrivateCallList>)·긴급 대상·인가를 읽는다.
검사: XSD 요소 집합/순서(OnNetworkType 에 MCPTTUserID 없음, EntryType 은 uri-entry 필수), 그룹 목록 = 소속 그룹
(소유만 한 그룹 제외, 소유 소속 그룹은 cims:authorized-user), ImplicitAffiliations = 내 멤버 행의 implicit_affiliation 이
켜진 그룹만(남의 설정은 안 따름, 없으면 요소 생략), 연락처 = 동료 멤버,
긴급 요소는 항상 존재(§8.3.2.1 shall) — 미지정은 entry-info 폴백 + ruleset 미인가, ProSe User-Info-ID 영값, 루트 Status,
선택이지만 필수로 읽는 단말용으로 항상 싣는 것(alias-entry index·xml:lang, ParticipantType — xml:lang 은 Name 과 같은 값),
common-policy ruleset, escape, ETag 내용 파생, 단말 정규식 호환(첫 MCPTTGroupInitiation = EmergencyCall),
ruleset anyExt 의 미응답 멤버 알림 자격(allow-to-receive-non-acknowledged-users-information, TS 24.379 §6.3.3.3),
ruleset 해제 인가 셋(allow-cancel-group-emergency·allow-cancel-imminent-peril·allow-cancel-emergency-alert — 목록 순서·
대상 결정과 AND 하지 않음·부재 시 값) — admin API 프로파일 GET/PUT 의 선택 컬럼 규약(부재 = 부재 시 값·입력 400)과 캐시 반영까지,
service-config 문서의 on-network <emergency-call><group-time-limit>(TNG2 — 0 이면 요소 생략)·<private-call>(개별 호 T4·최대 시간)·
<anyExt><adhoc-group-call>(애드혹 지원·인원·T4·TNG3 — mcptt_timers.md §7 D5·D6).

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
        self.assertEqual(common.find("up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "5",
                         "N6 = 그 밖 단말 기본 5 (DB 없음 = 역할 배정 없음, D2)")
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
        # PTT 그룹 호 청취 자격 = CIMS 확장(규격 ambient listening 과 다른 개념 — TS 24.484 에 이 이름이 없다)
        self.assertIsNone(acts.find("up:allow-ambient-listening", NS))
        self.assertEqual(acts.find("cims:allow-ambient-listening", NS).text, "true")
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
                         ["allow-to-receive-private-call-from-any-user", "allow-to-receive-non-acknowledged-users-information",
                          "allow-adhoc-group-call", "allow-adhoc-group-call-participation",
                          "allow-to-modify-adhoc-group-call-participants-info"],
                         "anyExt 자식 순서 = §8.3.2.1 11)xxxviii) 목록 순(K → L → R → S → AA)")
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, allow_non_ack_users_info=True)
        _, root, _ = self._doc()
        self.assertEqual(root.find("cp:ruleset/cp:rule/cp:actions/up:anyExt/"
                                   "up:allow-to-receive-non-acknowledged-users-information", NS).text, "true")
        # 옛 캐시 항목(키 없음)도 false
        m.PTT_PROFILES["+82500000001"] = {k: v for k, v in m.DEFAULT_USER_PROFILE.items() if k != "allow_non_ack_users_info"}
        _, root, _ = self._doc()
        self.assertEqual(root.find(".//up:allow-to-receive-non-acknowledged-users-information", NS).text, "false")

    ACTIONS_ORDER = ["allow-private-call", "allow-manual-commencement", "allow-automatic-commencement", "allow-force-auto-answer",
                     "allow-emergency-group-call", "allow-emergency-private-call", "allow-cancel-group-emergency",
                     "allow-cancel-private-emergency-call", "allow-imminent-peril-call",
                     "allow-cancel-imminent-peril", "allow-activate-emergency-alert", "allow-cancel-emergency-alert",
                     "allow-private-call-to-any-user", "allow-private-call-participation",
                     "anyExt", "allow-adhoc-group-call", "allow-create-group", "allow-ambient-listening"]

    def _acts(self):
        _, root, _ = self._doc()
        return root.find("cp:ruleset/cp:rule/cp:actions", NS)

    def test_cancel_authorisation_order_and_defaults(self):
        # TS 24.484 §8.3.2.1 11) 목록 순(vii~x 개별 호 → xii~xix 긴급·임박·경보 → xxvii·xxix 개별 호 any-user·착신) → anyExt,
        #   그 뒤 cims 확장(애드혹 별칭·그룹 생성·청취)
        acts = self._acts()
        self.assertEqual([c.tag.split("}")[1] for c in acts], self.ACTIONS_ORDER)
        self.assertEqual([c.tag.split("}")[0][1:] for c in acts][-3:], [NS["cims"]] * 3)
        # 기본값 — 그룹 긴급 해제 false(개시자만, local policy) · 임박 위험 해제 true · 경보 취소 = 발령 인가(기본 true).
        #   해제 인가는 대상 결정과 AND 하지 않는다 — 전용 긴급그룹 미지정으로 발령 인가가 false 여도 취소 인가는 true.
        self.assertEqual(acts.find("up:allow-cancel-group-emergency", NS).text, "false")
        self.assertEqual(acts.find("up:allow-cancel-imminent-peril", NS).text, "true")
        self.assertEqual(acts.find("up:allow-activate-emergency-alert", NS).text, "false", "DedicatedGroup 미지정 → 발령 미인가")
        self.assertEqual(acts.find("up:allow-cancel-emergency-alert", NS).text, "true", "취소 인가는 대상 결정과 무관")

    def test_cancel_authorisation_values(self):
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, emergency_group_id="g001",
                                             allow_cancel_group_emergency=True, allow_cancel_imminent_peril=False,
                                             allow_emergency_alert=True, allow_cancel_emergency_alert=False)
        acts = self._acts()
        self.assertEqual(acts.find("up:allow-cancel-group-emergency", NS).text, "true")
        self.assertEqual(acts.find("up:allow-cancel-imminent-peril", NS).text, "false")
        self.assertEqual(acts.find("up:allow-activate-emergency-alert", NS).text, "true")
        self.assertEqual(acts.find("up:allow-cancel-emergency-alert", NS).text, "false", "발령과 취소는 따로 — 발령만 주는 배정")

    def test_cancel_authorisation_absent_keys(self):
        # 옛 캐시 항목(키 없음) — 그룹 긴급 해제 false · 임박 위험 해제 true · 경보 취소 = 발령 인가 값(종전 문서와 같은 값)
        base = {k: v for k, v in m.DEFAULT_USER_PROFILE.items() if not k.startswith("allow_cancel_")}
        for alert in (True, False):
            m.PTT_PROFILES["+82500000001"] = dict(base, emergency_group_id="g001", allow_emergency_alert=alert)
            acts = self._acts()
            self.assertEqual(acts.find("up:allow-cancel-group-emergency", NS).text, "false")
            self.assertEqual(acts.find("up:allow-cancel-imminent-peril", NS).text, "true")
            self.assertEqual(acts.find("up:allow-cancel-emergency-alert", NS).text, "true" if alert else "false")

    def test_user_profile_config_and_etag(self):
        _, root, etag1 = self._doc()
        m.USER_PROFILE_CONFIG.update({"MissionCriticalOrganization": "포인티 <PS>"})
        m.SERVICE_CONFIG["max_calls_n6"] = 3
        xml, root2, etag2 = self._doc()
        self.assertEqual(root2.find("up:Common/up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "3",
                         "N6 = mcptt_service_config.max_calls_n6 (콘솔 MCPTT 정책)")
        self.assertEqual(root2.find("up:Common/up:MissionCriticalOrganization", NS).text, "포인티 <PS>")
        self.assertNotEqual(etag1, etag2, "ETag 는 내용 파생")
        m.GROUPS["tel:g002"]["members"].append({"uri": OUTSIDER, "name": "외부인"})
        _, _, etag3 = self._doc()
        self.assertNotEqual(etag2, etag3, "멤버십 변화도 ETag 변화(연락처)")

    def test_unknown_user(self):
        self.assertEqual(m.get_user_profile_xml("tel:+0"), (None, None))

    def test_private_call_authorisation_elements(self):
        # CMS-3 — 개별 호·임박 위험·애드혹 참가 인가 요소는 없으면 false(TS 24.484 표 8.3.2.7-7·-16·-27·-29·-48)라 늘 싣는다
        acts = self._acts()
        t = lambda p: acts.find(p, NS).text
        self.assertEqual([t("up:allow-private-call"), t("up:allow-manual-commencement"), t("up:allow-automatic-commencement"),
                          t("up:allow-force-auto-answer")], ["true", "true", "true", "false"])
        self.assertEqual([t("up:allow-private-call-to-any-user"), t("up:allow-private-call-participation"),
                          t("up:anyExt/up:allow-to-receive-private-call-from-any-user")], ["true", "true", "true"])
        self.assertEqual([t("up:anyExt/up:allow-adhoc-group-call-participation"),
                          t("up:anyExt/up:allow-to-modify-adhoc-group-call-participants-info")], ["true", "false"])
        # 임박 위험 호 = 긴급 그룹 호와 한 게이트(DedicatedGroup 미지정 → 둘 다 false), 긴급 사설콜 해제 = 개시 인가 값
        self.assertEqual((t("up:allow-emergency-group-call"), t("up:allow-imminent-peril-call")), ("false", "false"))
        self.assertEqual(t("up:allow-cancel-private-emergency-call"), "true")
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, emergency_group_id="g001", allow_private_call=False,
                                             allow_private_call_participation=False, allow_emergency_private_call=False)
        acts = self._acts()
        t = lambda p: acts.find(p, NS).text
        self.assertEqual((t("up:allow-emergency-group-call"), t("up:allow-imminent-peril-call")), ("true", "true"))
        self.assertEqual([t("up:allow-private-call"), t("up:allow-manual-commencement"), t("up:allow-automatic-commencement"),
                          t("up:allow-private-call-to-any-user")], ["false"] * 4, "발신 미인가면 개시 방식·any-user 도 false")
        self.assertEqual([t("up:allow-private-call-participation"),
                          t("up:anyExt/up:allow-to-receive-private-call-from-any-user")], ["false", "false"])
        self.assertEqual(t("up:allow-cancel-private-emergency-call"), "false")
        m.PTT_PROFILES["+82500000001"] = dict(m.DEFAULT_USER_PROFILE, allow_private_call_to_any_user=False)
        self.assertEqual(self._acts().find("up:allow-private-call-to-any-user", NS).text, "false", "목록(PrivateCallList) 한정")
        # 옛 캐시 항목(키 없음) = 허용(열 부재 시 값 1)
        m.PTT_PROFILES["+82500000001"] = {k: v for k, v in m.DEFAULT_USER_PROFILE.items() if not k.startswith("allow_private")}
        self.assertEqual(self._acts().find("up:allow-private-call", NS).text, "true")

    def test_n6_dispatch_role_vs_others(self):
        # D2 — N6 = 사용자마다의 값(TS 24.484 §8.3.2.1 8)e)i)): 관제(역할 배정) = max_calls_n6_dispatch, 그 밖 = max_calls_n6.
        #   판정 SQL 은 CSP(DbManager::LoadAllRoles → CCspRoleMap::SelectForLine)와 같은 펼침이다.
        m.SERVICE_CONFIG.update({"max_calls_n6": 4, "max_calls_n6_dispatch": 12})
        seen = []
        keep = m.is_dispatch_line
        try:
            m.is_dispatch_line = lambda msisdn: seen.append(msisdn) or True
            _, root, etag_d = self._doc()
            self.assertEqual(root.find("up:Common/up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "12")
            self.assertEqual(seen, ["+82500000001"], "판정은 문서 주인의 PTT 회선(msisdn)으로")
            m.is_dispatch_line = lambda msisdn: False
            _, root, etag_o = self._doc()
            self.assertEqual(root.find("up:Common/up:MCPTT-group-call/up:MaxSimultaneousCallsN6", NS).text, "4")
            self.assertNotEqual(etag_d, etag_o)
            m.SERVICE_CONFIG.pop("max_calls_n6")                  # 열 없는 DB = 코드 기본값
            self.assertEqual(m.user_max_calls_n6("+82500000001"), 5)
        finally:
            m.is_dispatch_line = keep
        self.assertIn("role_assignments a JOIN roles r", m._DISPATCH_LINE_SQL)
        self.assertIn("CAST(s.user_id AS CHAR) = a.principal_id", m._DISPATCH_LINE_SQL)
        self.assertFalse(m.is_dispatch_line("+82500000001"), "DB 없음 = 그 밖 단말")

    def test_entries_have_index_and_group_info_always(self):
        # §8.3.2.1 «The <entry> elements: 2) shall contain an "index" attribute» · 10)b) <MCPTTGroupInfo> 하나
        _, root, _ = self._doc()
        entries = list(root.iter("{%s}entry" % NS["up"]))
        self.assertTrue(entries)
        for e in entries:
            self.assertTrue(e.get("index"), "모든 <entry> 에 index")
        self.assertEqual(root.find(".//up:ProSeUserID-entry", NS).get("index"), "1", "§8.3.2.1 ProSeUserID-entry 도 index 필수")
        idx = [e.get("index") for e in root.findall("up:OnNetwork/up:MCPTTGroupInfo/up:entry", NS)]
        self.assertEqual(idx, ["1", "2"], "목록 안에서 유일")
        m.GROUPS.clear()
        _, root, _ = self._doc()
        gi = root.findall("up:OnNetwork/up:MCPTTGroupInfo", NS)
        self.assertEqual(len(gi), 1, "소속 그룹이 없어도 <MCPTTGroupInfo> 는 하나 있다")
        self.assertEqual(list(gi[0]), [])


class _ProfCur:
    """admin 프로파일 GET/PUT 이 내는 SQL 만 흉내 내는 DictCursor — cols = 선택 컬럼 중 DB 에 있는 것."""

    _CANCEL = ("allow_cancel_group_emergency", "allow_cancel_imminent_peril", "allow_cancel_emergency_alert")

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
                # 부재 컬럼 = 별칭 «<식> AS <열>» — 상수 0(자격 없음)·1(규격 기본 허용 — 임박 위험 해제·개별 호 인가 셋)·
                #   다른 열(경보 취소 = 발령 인가 값)
                for expr, c in re.findall(r"(\w+) AS (allow_\w+)", s):
                    r[c] = int(expr) if expr in ("0", "1") else r[expr]
                for c in m.USER_PROFILE_OPT_COLS:
                    if c not in r:
                        r[c] = 1 if m.USER_PROFILE_OPT_ABSENT_SQL.get(c) == "1" else 0   # 있는 열인데 시험 행에 없는 값
                for c in self._CANCEL:
                    if c not in r:
                        raise AssertionError(f"{c} 가 SELECT 에 없다: {s}")
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


class _AdminProfileBase(unittest.TestCase):
    """admin API `/users/{pid}/ptt/{msisdn}/profile` 시험 공통 — 가짜 DB 연결·캐시 보존."""

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


class AdminProfileNonAckTest(_AdminProfileBase):
    """admin API 프로파일의 allow_non_ack_users_info — 선택 컬럼 규약
    (부재 = 응답 false·입력 400 schema_not_migrated) + 캐시 반영 → user-profile anyExt."""

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


_ALL_OPT = {"allow_ambient_listening", "allow_create_group", "allow_non_ack_users_info",
            "allow_cancel_group_emergency", "allow_cancel_imminent_peril", "allow_cancel_emergency_alert"}
_PRE_CANCEL = _ALL_OPT - set(_ProfCur._CANCEL)


class AdminProfileCancelAuthzTest(_AdminProfileBase):
    """admin API 프로파일의 해제 인가 셋 — 선택 컬럼 규약(migrate_ptt_user_profile_cancel_authz.sql) + 부재 시 값
    (그룹 긴급 0 · 임박 위험 1 · 경보 취소 = 발령 인가) + 캐시 반영 → user-profile 문서."""

    def _row(self, **kw):
        row = {"allow_emergency_call": 1, "allow_emergency_alert": 1, "allow_adhoc_call": 1,
               "allow_emergency_private_call": 1, "emergency_group_mode": "DedicatedGroup", "emergency_group_id": "g001",
               "private_emergency_mode": "LocallyDetermined", "emergency_private_recipient": None,
               "allow_ambient_listening": 0, "allow_create_group": 0, "allow_non_ack_users_info": 0,
               "allow_cancel_group_emergency": 1, "allow_cancel_imminent_peril": 0, "allow_cancel_emergency_alert": 0}
        row.update(kw)
        return row

    def test_put_writes_cancel_columns_and_profile_doc(self):
        cur = _ProfCur(_ALL_OPT)
        self._with(cur)
        body = {"allow_emergency_alert": True, "allow_cancel_group_emergency": True,
                "allow_cancel_imminent_peril": False, "allow_cancel_emergency_alert": False}
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, body, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual({c: cur.inserted.get(c) for c in _ProfCur._CANCEL},
                         {"allow_cancel_group_emergency": 1, "allow_cancel_imminent_peril": 0, "allow_cancel_emergency_alert": 0})
        self.assertEqual((r.body["allow_cancel_group_emergency"], r.body["allow_cancel_imminent_peril"],
                          r.body["allow_cancel_emergency_alert"]), (True, False, False))
        xml, _ = m.get_user_profile_xml(ME)
        acts = ET.fromstring(xml.encode()).find("cp:ruleset/cp:rule/cp:actions", NS)
        self.assertEqual(acts.find("up:allow-cancel-group-emergency", NS).text, "true")
        self.assertEqual(acts.find("up:allow-cancel-imminent-peril", NS).text, "false")
        self.assertEqual(acts.find("up:allow-cancel-emergency-alert", NS).text, "false")

    def test_put_omitted_keys_take_absent_values(self):
        for alert in (True, False):
            self.adm._OPT_COL_PRESENT.clear()
            cur = _ProfCur(_ALL_OPT)
            self._with(cur)
            r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_emergency_alert": alert}, {}))
            self.assertEqual(r.status, 200, r.body)
            self.assertEqual({c: cur.inserted.get(c) for c in _ProfCur._CANCEL},
                             {"allow_cancel_group_emergency": 0, "allow_cancel_imminent_peril": 1,
                              "allow_cancel_emergency_alert": 1 if alert else 0}, "경보 취소 = 이 요청의 발령 인가")

    def test_put_absent_columns_is_400_with_migration_hint(self):
        for c in _ProfCur._CANCEL:
            self.adm._OPT_COL_PRESENT.clear()
            self._with(_ProfCur(_PRE_CANCEL))
            r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {c: True}, {}))
            self.assertEqual(r.status, 400, c)
            self.assertEqual(r.body["error"], "schema_not_migrated")
            self.assertIn("migrate_ptt_user_profile_cancel_authz.sql", r.body["detail"])

    def test_put_absent_columns_without_key_is_ok(self):
        cur = _ProfCur(_PRE_CANCEL)
        self._with(cur)
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_emergency_alert": False}, {}))
        self.assertEqual(r.status, 200, r.body)
        for c in _ProfCur._CANCEL:
            self.assertNotIn(c, cur.inserted, "부재 컬럼은 INSERT 에 싣지 않는다")
        self.assertEqual((r.body["allow_cancel_group_emergency"], r.body["allow_cancel_imminent_peril"],
                          r.body["allow_cancel_emergency_alert"]), (False, True, False), "응답 = 부재 시 값")
        self.assertIs(m.PTT_PROFILES[self.MSISDN]["allow_cancel_emergency_alert"], False, "캐시도 같은 값")

    def test_get_absent_columns_read_absent_values(self):
        row = self._row(allow_emergency_alert=0)
        self._with(_ProfCur(_PRE_CANCEL, row=row))
        r = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertEqual(r.status, 200)
        self.assertEqual((r.body["allow_cancel_group_emergency"], r.body["allow_cancel_imminent_peril"],
                          r.body["allow_cancel_emergency_alert"]), (False, True, False),
                         "부재 = 0 · 1 · allow_emergency_alert (SELECT 대체식)")
        self.adm._OPT_COL_PRESENT.clear()
        self._with(_ProfCur(_ALL_OPT, row=row))
        r = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertEqual((r.body["allow_cancel_group_emergency"], r.body["allow_cancel_imminent_peril"],
                          r.body["allow_cancel_emergency_alert"]), (True, False, False), "컬럼이 있으면 행 값")

    def test_get_without_row_is_default(self):
        self._with(_ProfCur(_ALL_OPT, row=None))
        r = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertIs(r.body["exists"], False)
        self.assertEqual((r.body["allow_cancel_group_emergency"], r.body["allow_cancel_imminent_peril"],
                          r.body["allow_cancel_emergency_alert"]), (False, True, True))

    def test_default_profile_and_absent_rule(self):
        d = m.DEFAULT_USER_PROFILE
        self.assertEqual((d["allow_cancel_group_emergency"], d["allow_cancel_imminent_peril"], d["allow_cancel_emergency_alert"]),
                         (False, True, True))
        self.assertIs(m.user_profile_opt_default("allow_cancel_emergency_alert", {"allow_emergency_alert": False}), False)
        self.assertIs(m.user_profile_opt_default("allow_cancel_imminent_peril", {}), True)
        self.assertIs(m.user_profile_opt_default("allow_non_ack_users_info", {}), False)


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
        # 스키마 시퀀스(on-networkType) — private-call · transmit-time · fc-timers-counters · RP 셋 · anyExt, 그 순서
        tags = [c.tag.split("}")[1] for c in on]
        self.assertEqual(tags, ["private-call", "transmit-time", "fc-timers-counters", "signalling-protection",
                                "protection-between-mcptt-servers", "emergency-resource-priority",
                                "imminent-peril-resource-priority", "normal-resource-priority", "anyExt"])
        # 서버 간 보호 둘도 없으면 true(§8.4.2.6 NOTE 4) — false 명시
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in on.find("sc:protection-between-mcptt-servers", SC)],
                         [("allow-signalling-protection", "false"), ("allow-floor-control-protection", "false")])
        # <signalling-protection> 둘은 없으면 true(§8.4.2.6) — 단말이 mcptt-info 를 암호화·서명하지 않게 false 를 명시한다
        #   (TS 24.379 §6.6.2.3.1·§6.6.3.3.1)
        sp = on.find("sc:signalling-protection", SC)
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in sp],
                         [("confidentiality-protection", "false"), ("integrity-protection", "false")])
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

    def test_emergency_call_group_time_limit(self):
        """on-network <emergency-call><group-time-limit> — TNG2(TS 24.379 §6.3.3.1.16). 스키마 순 첫 자식, 0 이면 생략."""
        _, etag0 = self._doc()
        for v, want in ((90000, "PT90S"), (1500, "PT1.5S"), ("600000", "PT600S")):
            m.SERVICE_CONFIG_PARAMS["EmergencyCall"] = {"GroupTimeLimit": v}
            root, etag = self._doc()
            on = root.find("sc:service-configuration-params/sc:on-network", SC)
            self.assertEqual(on[0].tag.split("}")[1], "emergency-call", "on-network 시퀀스의 첫 자식")
            self.assertEqual([c.tag.split("}")[1] for c in on[0]], ["group-time-limit"])
            self.assertEqual(on.find("sc:emergency-call/sc:group-time-limit", SC).text, want)
            self.assertEqual(on[1].tag.split("}")[1], "private-call", "다음 자식 = private-call(§8.4.2.3 순서)")
            self.assertNotEqual(etag0, etag)
        for v in (0, "", None, "abc", -5):
            m.SERVICE_CONFIG_PARAMS["EmergencyCall"] = {"GroupTimeLimit": v}
            root, etag = self._doc()
            self.assertIsNone(root.find(".//sc:emergency-call", SC), f"{v!r} → 요소째 생략(TNG2 미가동)")
            self.assertEqual(etag, etag0, "생략 = 기본 문서와 같다")
        m.SERVICE_CONFIG_PARAMS.pop("EmergencyCall")
        self.assertEqual(m._SERVICE_CONFIG_PARAM_DEFAULTS["EmergencyCall"]["GroupTimeLimit"], 0, "기본 = 없음")

    def test_private_call_timers(self):
        """on-network <private-call> — 개별 호 T4(<hang-time>, TS 24.380 표 11.1.3-1)·최대 시간(TS 24.379 §6.3.8.2 2)).
        privateType 순서 = hang-time · max-duration-with-floor-control · max-duration-without-floor-control, 0 인 자식은 생략,
        셋 다 0 이면 요소째 생략."""
        root, etag0 = self._doc()
        pc = root.find("sc:service-configuration-params/sc:on-network/sc:private-call", SC)
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in pc],
                         [("hang-time", "PT30S"), ("max-duration-with-floor-control", "PT3600S"),
                          ("max-duration-without-floor-control", "PT3600S")], "기본값 — T4 규격 기본 30초")
        m.SERVICE_CONFIG_PARAMS["PrivateCall"] = {"HangTime": 15000, "MaxDurationWithFloorControl": 0,
                                                  "MaxDurationWithoutFloorControl": "1800000"}
        root, etag = self._doc()
        pc = root.find("sc:service-configuration-params/sc:on-network/sc:private-call", SC)
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in pc],
                         [("hang-time", "PT15S"), ("max-duration-without-floor-control", "PT1800S")])
        self.assertNotEqual(etag, etag0)
        m.SERVICE_CONFIG_PARAMS["PrivateCall"] = {"HangTime": 0, "MaxDurationWithFloorControl": 0,
                                                  "MaxDurationWithoutFloorControl": "x"}
        root, _ = self._doc()
        self.assertIsNone(root.find(".//sc:private-call", SC), "자식이 없으면 요소째 생략")
        self.assertNotIn("PT0S</hang-time>", m.get_service_config_xml(None)[0])

    def test_adhoc_group_call(self):
        """on-network <anyExt><adhoc-group-call> (TS 24.484 §8.4.2.1 13)d)·§8.4.2.3 adhoc-group-callType) — 필수 자식
        allow-adhoc-group-call-support·max-no-participants 뒤 hang-time(T4)·broadcast-hang-time·max-duration-of-call(TNG3,
        TS 24.379 §17.4.2.2 13)). 요소가 없으면 «애드혹 미지원»(§8.4.2.6)이라 늘 싣는다."""
        root, _ = self._doc()
        on = root.find("sc:service-configuration-params/sc:on-network", SC)
        self.assertEqual(on[-1].tag.split("}")[1], "anyExt", "anyExt 는 on-network 의 마지막 자식")
        ag = on.find("sc:anyExt/sc:adhoc-group-call", SC)
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in ag],
                         [("allow-adhoc-group-call-support", "true"), ("max-no-participants", "64"),
                          ("hang-time", "PT30S"), ("broadcast-hang-time", "PT30S"), ("max-duration-of-call", "PT3600S")])
        m.SERVICE_CONFIG_PARAMS["AdhocGroupCall"] = {"AllowSupport": "false", "MaxNoParticipants": 0, "HangTime": 0,
                                                     "BroadcastHangTime": 2500, "MaxDurationOfCall": 0}
        root, _ = self._doc()
        ag = root.find("sc:service-configuration-params/sc:on-network/sc:anyExt/sc:adhoc-group-call", SC)
        self.assertEqual([(c.tag.split("}")[1], c.text) for c in ag],
                         [("allow-adhoc-group-call-support", "false"), ("max-no-participants", "1"),
                          ("broadcast-hang-time", "PT2.5S")], "필수 둘은 남고(positiveInteger ≥ 1), 0 인 시간 요소는 생략")


if __name__ == "__main__":
    unittest.main()


class AdminProfilePrivateCallTest(_AdminProfileBase):
    """admin API 프로파일의 개별 호 인가 셋(CMS-3 — migrate_ptt_user_profile_private_call.sql) — 선택 컬럼 규약: 부재 시 값 1(허용),
    열이 없는 DB 에 키를 주면 400, 캐시 반영 → user-profile 문서."""
    COLS = ("allow_private_call", "allow_private_call_to_any_user", "allow_private_call_participation")

    def test_put_writes_columns_and_profile_doc(self):
        cur = _ProfCur(_ALL_OPT | set(self.COLS))
        self._with(cur)
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_private_call": False,
                                                                     "allow_private_call_participation": False}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual({c: cur.inserted.get(c) for c in self.COLS},
                         {"allow_private_call": 0, "allow_private_call_to_any_user": 1, "allow_private_call_participation": 0},
                         "본문에 없는 키 = 부재 시 값 1")
        acts = ET.fromstring(m.get_user_profile_xml(ME)[0].encode()).find("cp:ruleset/cp:rule/cp:actions", NS)
        self.assertEqual(acts.find("up:allow-private-call", NS).text, "false")
        self.assertEqual(acts.find("up:allow-private-call-participation", NS).text, "false")

    def test_absent_columns(self):
        for c in self.COLS:
            self.adm._OPT_COL_PRESENT.clear()
            self._with(_ProfCur(_ALL_OPT))
            r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {c: False}, {}))
            self.assertEqual((r.status, r.body["error"]), (400, "schema_not_migrated"))
            self.assertIn("migrate_ptt_user_profile_private_call.sql", r.body["detail"])
        self.adm._OPT_COL_PRESENT.clear()
        cur = _ProfCur(_ALL_OPT)
        self._with(cur)
        r = asyncio.run(self.adm._put_ptt_profile("7", self.MSISDN, {"allow_adhoc_call": False}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertNotIn("allow_private_call", cur.inserted, "없는 열은 쓰지 않는다")
        self.assertTrue(r.body["allow_private_call"], "열 없는 DB = 허용(부재 시 값)")
        self.adm._OPT_COL_PRESENT.clear()
        row = {"allow_emergency_call": 1, "allow_emergency_alert": 1, "allow_adhoc_call": 1, "allow_emergency_private_call": 1,
               "emergency_group_mode": "DedicatedGroup", "emergency_group_id": None,
               "private_emergency_mode": "LocallyDetermined", "emergency_private_recipient": None}
        self._with(_ProfCur(set(), row))
        g = asyncio.run(self.adm._get_ptt_profile("7", self.MSISDN, {}))
        self.assertEqual([g.body[c] for c in self.COLS], [True, True, True])


class _SvcCur:
    """admin `/mcptt/service-config` GET/PUT SQL 흉내 — have = mcptt_service_config 에 있는 선택 열(N6)."""

    def __init__(self, have, row=None):
        self.have, self.row, self.upsert = set(have), row, None
        self._rows = []

    def execute(self, sql, params=None):
        s = " ".join(sql.split())
        self._rows = []
        if s.startswith("SHOW COLUMNS FROM mcptt_service_config LIKE 'max_calls_n6%'"):
            self._rows = [{"Field": c} for c in sorted(self.have)]
        elif s.startswith("SELECT ") and "FROM mcptt_service_config WHERE id=1" in s:
            cols = [c.strip() for c in s[len("SELECT "):s.index(" FROM")].split(",")]
            for c in cols:
                if c.startswith("max_calls_n6") and c not in self.have:
                    raise AssertionError(f"없는 열을 읽는다: {c}")
            self._rows = [{c: self.row.get(c) for c in cols}] if self.row else []
        elif s.startswith("INSERT INTO mcptt_service_config"):
            head = s.split("(", 1)[1].split(")", 1)[0]
            self.upsert = dict(zip([c.strip() for c in head.split(",")][1:], params))
        else:
            raise AssertionError(f"unexpected SQL: {s}")

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class AdminServiceConfigN6Test(unittest.TestCase):
    """D2 — N6 두 값(관제·그 밖)은 mcptt_service_config 선택 열(migrate_mcptt_n6.sql). 열 없는 DB 는 코드 기본값 10·5 를 돌려주고,
    그 값을 바꾸려는 PUT 은 400(마이그레이션 안내) — 다른 값의 PUT 은 그대로 된다."""

    @classmethod
    def setUpClass(cls):
        import handlers.admin as adm
        cls.adm = adm

    def setUp(self):
        self._keep = (dict(m.SERVICE_CONFIG), self.adm._get_db, self.adm.notify_csp)
        self.notified = []
        self.adm.notify_csp = lambda *a, **k: self.notified.append(a[0])

    def tearDown(self):
        sc, get_db, notify = self._keep
        m.SERVICE_CONFIG.clear(); m.SERVICE_CONFIG.update(sc)
        self.adm._get_db, self.adm.notify_csp = get_db, notify

    def _with(self, cur):
        self.adm._get_db = lambda config: _ProfConn(cur)

    ROW = {"max_affiliations_n2": 10, "num_levels_group_hierarchy": 3, "num_levels_user_hierarchy": 3,
           "max_calls_n6": 6, "max_calls_n6_dispatch": 16, "update_time": None}

    def test_get_with_and_without_columns(self):
        self._with(_SvcCur(m.SERVICE_CONFIG_OPT_COLS, self.ROW))
        r = asyncio.run(self.adm._get_mcptt_service_config({}))
        self.assertEqual((r.body["max_calls_n6"], r.body["max_calls_n6_dispatch"]), (6, 16))
        self._with(_SvcCur((), self.ROW))
        r = asyncio.run(self.adm._get_mcptt_service_config({}))
        self.assertEqual((r.body["max_calls_n6"], r.body["max_calls_n6_dispatch"]), (5, 10), "열 없음 = 기본값(D2)")
        self.assertTrue(r.body["exists"])

    def test_put_range_columns_and_notify(self):
        cur = _SvcCur(m.SERVICE_CONFIG_OPT_COLS, self.ROW)
        self._with(cur)
        r = asyncio.run(self.adm._put_mcptt_service_config({"max_calls_n6": 0}, {}))
        self.assertEqual(r.status, 400, "N6 = xs:positiveInteger")
        r = asyncio.run(self.adm._put_mcptt_service_config({"max_calls_n6": 3, "max_calls_n6_dispatch": 20}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual((cur.upsert["max_calls_n6"], cur.upsert["max_calls_n6_dispatch"]), (3, 20))
        self.assertEqual(m.SERVICE_CONFIG["max_calls_n6"], 3)
        self.assertIn("SERVICE_CONFIG_CHANGED", self.notified, "cms 구독자 전원 xcap-diff — user-profile 도 다시 받는다")

    def test_put_without_columns(self):
        cur = _SvcCur((), self.ROW)
        self._with(cur)
        r = asyncio.run(self.adm._put_mcptt_service_config({"max_calls_n6_dispatch": 20}, {}))
        self.assertEqual(r.status, 400)
        self.assertIn("migrate_mcptt_n6.sql", r.body["error"])
        r = asyncio.run(self.adm._put_mcptt_service_config({"max_affiliations_n2": 8}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertNotIn("max_calls_n6", cur.upsert, "없는 열은 쓰지 않는다")
        self.assertEqual(cur.upsert["max_affiliations_n2"], 8)


class AdminGroupPriorityTest(unittest.TestCase):
    """GMS-18 — 그룹·멤버 우선순위 = priorityType 0..255(TS 24.481 §7.2.4.2). 관리 API 쓰기 경로가 범위 밖을 400 으로 거절한다."""

    def test_check_priorities(self):
        import handlers.admin as adm
        self.assertIsNone(adm._check_priorities({"priority": 255, "members": [{"user_id": "+8201", "priority": 0}]}))
        self.assertIsNone(adm._check_priorities({}))
        self.assertIn("priority", adm._check_priorities({"priority": 256}))
        self.assertIn("members[+8201]", adm._check_priorities({"members": [{"user_id": "+8201", "priority": -1}]}))
        self.assertIsNotNone(adm._check_priorities({"priority": "x"}))
        r = asyncio.run(adm._add_member("g001", {"user_id": "+8201", "priority": 300}, {}))
        self.assertEqual(r.status, 400, "멤버 추가도 같은 검사(DB 전에)")
