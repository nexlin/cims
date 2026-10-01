"""csc/src/services/mcvideo.py — MCVideo 설정 평면 단위 시험 (오프라인, DB 없음) · 계약 K2 골든 문서의 생성 쪽 시험.

설계 정본 docs/design/features/mcvideo.md §5.1, 계약 docs/dev/mcvideo_dev_plan.md §3 K1·K2. 골든 = tests/fixtures/mcvideo/*.xml —
CSC 가 아래 FIXTURE_SCENARIO 로 내는 문서와 **같아야** 한다(정규화 비교). 단말 SDK(.45)는 같은 파일을 읽어 README 의 값을 확인한다.
검사: 그룹 문서 V0(MCPTT enabler = MCPTT ICSI·규칙 is-list-member/allow-initiate-conference/join-handling) + MCVideo 몫(<service>·
mcvideo-* 속성·entry <mcvideo-mcvideo-id>·보호 false 명시), MCPTT 전용 그룹엔 MCVideo 요소 없음, XCAP PUT 해석(MCVideo <service> 있음 →
켬·속성 / 없음 → 기존 상태 유지 전환기 규칙 / 보호 true·범위 밖 400), DB 쓰기 SQL 조립, MCVideo user profile(자격 행·MCVideo 그룹만)·
service config(설정 반영·보호 false)·ue-init-config MCVideo-Service-Details, CMS 핸들러 인가(scope·본인·자격 404), 토큰 mcvideo_id claim,
관리 API(A6 — 그룹 `mcvideo` 키 없음/null/객체 · 쓰기 전 검사 · 마이그레이션 전 400 · PTT 회선 MCVideo 자격 GET/PUT/DELETE·캐시·통지).

  python3 -m unittest tests.test_csc_mcvideo
  python3 tests/test_csc_mcvideo.py --write-fixtures     # 골든 재기록(계약 변경 때만 — 커밋 전에 .45 와 합의)
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest
import xml.etree.ElementTree as ET

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))
for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
    if os.path.isdir(_v) and _v not in sys.path:
        sys.path.append(_v)
        break

import jwt  # noqa: E402
import services.mcptt as m  # noqa: E402
import services.mcvideo as mv  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

FIX = os.path.join(_REPO_ROOT, "tests", "fixtures", "mcvideo")
DOMAIN = "ptt.cims.example.kr"
BASE_URL = "https://csc.ptt.cims.example.kr:4430"
A, B, C = "tel:+82510002001", "tel:+82510002002", "tel:+82510002003"
NS = {"poc": "urn:oma:xml:poc:list-service", "gi": mv.NS_GI, "oxe": "urn:oma:xml:xdm:extensions",
      "cp": "urn:ietf:params:xml:ns:common-policy", "up": mv.NS_USER_PROFILE, "sc": mv.NS_SERVICE_CONFIG,
      "ue": "urn:3gpp:mcptt:mcpttUEinitConfig:1.0"}


def _group(name, mcvideo):
    return {"display_name": name, "priority": 5, "encryption": False,
            "emergency_call": True, "emergency_alert": True, "allow_conference_state": True, "allow_sds": True,
            "allow_fd": False, "max_sds_size": 10000, "max_auto_recv": 1048576, "org_code": "",
            "group_type": "prearranged", "max_members": 0, "require_affiliation": True, "hang_timer_sec": 30,
            "max_duration_sec": 3600, "min_number_to_start": 0, "ack_timeout_sec": 5, "ack_action": "abandon",
            "authorized_user": "", "authorized_user_id": None, "mcvideo": mcvideo,
            "members": [{"uri": A, "name": "영상요원1", "role": "participant", "priority": 5},
                        {"uri": B, "name": "영상요원2", "role": "participant", "priority": 5},
                        {"uri": C, "name": "관제1", "role": "chair", "priority": 1}]}


def FIXTURE_SCENARIO():
    """골든 문서의 입력 — README.md §시나리오 와 같다. 반환 = 복원용 사본."""
    from services import access_services as _acs
    keep = (dict(m.USERS), dict(m.GROUPS), dict(mv.MCVIDEO_PROFILES), dict(m.SERVICE_CONFIG), m.UE_INIT_CONFIG,
            m.PROVISIONING, dict(m.USER_PROFILE_CONFIG), mv.SERVICE_CONFIG_PARAMS, _acs.ptt_domain)
    # PTT 도메인 = 시나리오 값으로 고정 — 접속서비스 미러(CSP 정본) 상태가 다른 시험에서 남아 있어도 골든이 흔들리지 않게.
    _acs.ptt_domain = lambda provisioning, config=None: DOMAIN
    m.USERS.clear(); m.GROUPS.clear(); mv.MCVIDEO_PROFILES.clear(); m.USER_PROFILE_CONFIG.clear()
    m.PROVISIONING = {"Services": {"ptt": {"domain": DOMAIN}}}
    for u, n in ((A, "영상요원1"), (B, "영상요원2"), (C, "관제1")):
        m.USERS[u] = {"msisdn": u[4:], "name": n, "password": ""}
    m.GROUPS["tel:g101"] = _group("현장영상 1팀", dict(mv.GROUP_ATTR_DEFAULTS, max_duration_sec=1800,
                                                       video_resolutions="1280x720,640x480", video_frame_rate="30,15",
                                                       group_priority=100))
    m.GROUPS["tel:g102"] = _group("음성 전용", None)
    mv.MCVIDEO_PROFILES.update({A[4:]: {"max_video_streams": 1, "max_calls_n6": 1},
                                B[4:]: {"max_video_streams": 1, "max_calls_n6": 1},
                                C[4:]: {"max_video_streams": 4, "max_calls_n6": 2}})
    m.SERVICE_CONFIG["max_affiliations_n2"] = 10
    m.UE_INIT_CONFIG = {"ServiceDetails": {"McVideo": {"Enable": True}, "McData": {"Enable": True}}}
    mv.SERVICE_CONFIG_PARAMS = {}
    return keep


def _restore(keep):
    from services import access_services as _acs
    users, groups, profs, sc, uic, prov, upc, mvsc, ptt_domain = keep
    _acs.ptt_domain = ptt_domain
    m.USERS.clear(); m.USERS.update(users); m.GROUPS.clear(); m.GROUPS.update(groups)
    mv.MCVIDEO_PROFILES.clear(); mv.MCVIDEO_PROFILES.update(profs)
    m.SERVICE_CONFIG.clear(); m.SERVICE_CONFIG.update(sc)
    m.UE_INIT_CONFIG, m.PROVISIONING, mv.SERVICE_CONFIG_PARAMS = uic, prov, mvsc
    m.USER_PROFILE_CONFIG.clear(); m.USER_PROFILE_CONFIG.update(upc)


GOLDEN = {
    "group_g101.xml": lambda: m.get_group_xml("tel:g101")[0],
    "group_g102_mcptt_only.xml": lambda: m.get_group_xml("tel:g102")[0],
    "mcvideo_user_profile.xml": lambda: mv.get_user_profile_xml(A)[0],
    "mcvideo_service_config.xml": lambda: mv.get_service_config_xml()[0],
    "ue_init_config.xml": lambda: m.get_ue_init_config_xml(BASE_URL)[0],
}


def _canon(text: str) -> str:
    return ET.canonicalize(xml_data=text, strip_text=True)


def _run(coro):
    return asyncio.run(coro)


class GoldenTest(unittest.TestCase):
    def setUp(self):
        self.keep = FIXTURE_SCENARIO()

    def tearDown(self):
        _restore(self.keep)

    def test_generated_equals_golden(self):
        for name, gen in GOLDEN.items():
            with self.subTest(name=name):
                with open(os.path.join(FIX, name), encoding="utf-8") as f:
                    self.assertEqual(_canon(gen()), _canon(f.read()),
                                     f"{name}: CSC 산출물이 계약 골든과 다르다 — 계약 변경이면 .45 와 합의 뒤 --write-fixtures")


class GroupDocumentTest(unittest.TestCase):
    def setUp(self):
        self.keep = FIXTURE_SCENARIO()

    def tearDown(self):
        _restore(self.keep)

    def _ls(self, gid):
        return ET.fromstring(m.get_group_xml(gid)[0]).find("poc:list-service", NS)

    def test_v0_mcptt_enabler_is_icsi_and_rule_authorises_members(self):
        for gid in ("tel:g101", "tel:g102"):
            ls = self._ls(gid)
            enablers = [s.get("enabler") for s in ls.iter(f"{{{NS['oxe']}}}service")]
            self.assertIn(mv.ICSI_MCPTT, enablers)
            self.assertNotIn("example.mcptt", enablers)
            rule = ls.find("cp:ruleset/cp:rule", NS)
            self.assertIsNotNone(rule.find("cp:conditions/poc:is-list-member", NS))
            self.assertEqual(rule.find("cp:actions/poc:allow-initiate-conference", NS).text, "true")
            self.assertEqual(rule.find("cp:actions/poc:join-handling", NS).text, "true")

    def test_mcvideo_group_carries_service_attrs_and_member_ids(self):
        ls = self._ls("tel:g101")
        svc = [s for s in ls.iter(f"{{{NS['oxe']}}}service") if s.get("enabler") == mv.ICSI_MCVIDEO]
        self.assertEqual(len(svc), 1)
        self.assertIsNotNone(svc[0].find("oxe:group-media/gi:mcvideo-video-media", NS))
        self.assertEqual(ls.find("gi:mcvideo-on-network-invite-members", NS).text, "false")   # chat (D5)
        self.assertEqual(ls.find("gi:mcvideo-protect-media", NS).text, "false")                # 명시 false (D7)
        self.assertEqual(ls.find("gi:mcvideo-protect-transmission-control", NS).text, "false")
        self.assertEqual(ls.find("gi:mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members", NS).text, "2")
        self.assertEqual([e.get("name") for e in ls.find("gi:mcvideo-preferred-video-encodings", NS)], ["H264"])
        self.assertEqual(ls.find("gi:on-network-reception-hang-timer", NS).text, "PT30S")
        for e in ls.find("poc:list", NS):
            self.assertEqual(e.find("gi:mcvideo-mcvideo-id", NS).get("uri"), e.get("uri"))     # D1 단일 ID
            self.assertEqual(e.find("gi:mcdata-mcdata-id", NS).get("uri"), e.get("uri"))       # §7.2.2 MCData entry c)

    def test_mcptt_only_group_has_no_mcvideo(self):
        xml = m.get_group_xml("tel:g102")[0]
        self.assertNotIn("mcvideo", xml)

    def test_put_roundtrip_reads_mcvideo(self):
        doc = m.parse_group_document_xml(m.get_group_xml("tel:g101")[0])
        self.assertIsNotNone(doc["mcvideo"])
        mvd = doc["mcvideo"]
        self.assertEqual(mvd["invite_members"], False)
        self.assertEqual(mvd["max_duration_sec"], 1800)
        self.assertEqual(mvd["max_transmitters"], 2)
        self.assertEqual(mvd["audio_encodings"], ["AMR-WB"])
        self.assertEqual(mvd["video_resolutions"], "1280x720,640x480")
        self.assertEqual(mvd["group_priority"], 100)
        self.assertEqual(mvd["allow_conference_state"], True)

    def test_put_without_mcvideo_service_keeps_state(self):
        """전환기 규칙 — MCVideo 를 모르는 단말(옛 SDK: enabler example.mcptt)의 PUT 은 MCVideo 를 건드리지 않는다."""
        old_sdk_doc = m.get_group_xml("tel:g102")[0].replace(mv.ICSI_MCPTT, "example.mcptt")
        doc = m.parse_group_document_xml(old_sdk_doc)
        self.assertIsNone(doc["mcvideo"])

    def test_put_rejects_protect_true_and_out_of_range(self):
        base = m.get_group_xml("tel:g101")[0]
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(base.replace(
                "<mcpttgi:mcvideo-protect-media>false<", "<mcpttgi:mcvideo-protect-media>true<"))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(base.replace(
                "transmitting-group-members>2<", "transmitting-group-members>0<"))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(base.replace(
                "<mcpttgi:on-network-reception-hang-timer>PT30S<", "<mcpttgi:on-network-reception-hang-timer>30s<"))

    def test_write_group_attrs_sql(self):
        class Cur:
            def __init__(self):
                self.q = []
                self.row = None

            def execute(self, sql, args=()):
                self.q.append((sql, args))

            def fetchone(self):
                return self.row
        cur = Cur()
        mv.write_group_attrs(cur, 42, {"invite_members": True, "max_transmitters": 3, "audio_encodings": ["AMR-WB"],
                                       "video_resolutions": None})
        sql, args = cur.q[-1]
        self.assertIn("INSERT INTO mcvideo_group_attrs", sql)
        self.assertIn("ON DUPLICATE KEY UPDATE", sql)
        self.assertEqual(args[0], 42)
        self.assertEqual(args[1], 1)          # invite_members
        self.assertEqual(args[3], 3)          # max_transmitters
        self.assertEqual(args[4], "AMR-WB")
        self.assertEqual(args[5], "H264")     # 기본값
        self.assertIsNone(args[6])
        cur2 = Cur()
        mv.write_group_attrs(cur2, 42, None)
        self.assertEqual(cur2.q[-1], ("DELETE FROM mcvideo_group_attrs WHERE group_id=%s", (42,)))


class _AdminCur:
    """관리 API 가 내는 SQL 만 흉내 내는 DictCursor — 실행한 SQL 을 기록한다."""

    def __init__(self, tables=True, attrs_row=None, profile_row=None, owns_line=True):
        self.tables, self.attrs_row, self.profile_row, self.owns_line = tables, attrs_row, profile_row, owns_line
        self.q = []
        self._rows = []
        self.rowcount = 0
        self.lastrowid = 77

    def execute(self, sql, args=()):
        s = " ".join(sql.split())
        self.q.append((s, args))
        self._rows, self.rowcount = [], 0
        if s == "SELECT id FROM ptt_groups WHERE mcptt_group_id=%s":
            self._rows = [{"id": 42}]
        elif s == "SHOW TABLES LIKE 'mcvideo_group_attrs'":
            self._rows = [{"t": "mcvideo_group_attrs"}] if self.tables else []
        elif s == "SELECT * FROM mcvideo_group_attrs WHERE group_id=%s":
            self._rows = [self.attrs_row] if self.attrs_row else []
        elif s == "SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s":
            self._rows = [{"1": 1}] if self.owns_line else []
        elif s.startswith("SELECT max_video_streams, max_calls_n6 FROM mcvideo_user_profile WHERE ptt_id=%s"):
            self._rows = [self.profile_row] if self.profile_row else []
        elif s.startswith("DELETE FROM mcvideo_user_profile"):
            self.rowcount = 1 if self.profile_row else 0
        elif s.startswith(("INSERT INTO", "UPDATE ptt_groups", "DELETE FROM mcvideo_group_attrs")):
            self.rowcount = 1
        else:
            raise AssertionError(f"unexpected SQL: {s}")

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)

    def sql(self, prefix):
        return [(s, a) for s, a in self.q if s.startswith(prefix)]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class _AdminConn:
    def __init__(self, cur):
        self.cur = cur

    def cursor(self):
        return self.cur

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class AdminApiTest(unittest.TestCase):
    """A6 관리 API — 그룹 `mcvideo`(키 없음 = 그대로 · null = 끔 · 객체 = 켬/갱신)·PTT 회선 MCVideo 자격 (mcvideo.md §5.1)."""

    @classmethod
    def setUpClass(cls):
        import handlers.admin as adm
        cls.adm = adm

    def setUp(self):
        self._keep = (self.adm._get_db, self.adm.notify_csp, self.adm.sync_group_from_db, dict(mv.MCVIDEO_PROFILES))
        self.notified = []
        self.adm.notify_csp = lambda *a, **k: self.notified.append(a)
        self.adm.sync_group_from_db = lambda gid: None
        self.cur = None

    def tearDown(self):
        self.adm._get_db, self.adm.notify_csp, self.adm.sync_group_from_db, profs = self._keep
        mv.MCVIDEO_PROFILES.clear(); mv.MCVIDEO_PROFILES.update(profs)

    def _db(self, **kw):
        self.cur = _AdminCur(**kw)
        self.adm._get_db = lambda config: _AdminConn(self.cur)
        return self.cur

    def test_api_group_attrs_validation(self):
        self.assertEqual(mv.api_group_attrs(None), (None, None))
        a, err = mv.api_group_attrs({"invite_members": True, "video_encodings": "H264, VP8", "video_resolutions": " "})
        self.assertIsNone(err)
        self.assertEqual(a["video_encodings"], ["H264", "VP8"], "쉼표 문자열도 목록으로")
        self.assertIsNone(a["video_resolutions"], "빈 문자열 = 요소 생략(None)")
        for bad in ("on", [], {"max_transmitters": 17}, {"max_transmitters": 0}, {"invite_members": "yes"},
                    {"protect_media": True}, {"protect_transmission_control": True}, {"bogus": 1},
                    {"audio_encodings": []}, {"group_priority": 256}, {"video_frame_rate": 30}):
            with self.subTest(bad=bad):
                self.assertIsNotNone(mv.api_group_attrs(bad)[1])

    def test_update_without_mcvideo_key_leaves_mcvideo(self):
        cur = self._db()
        r = _run(self.adm._update_group("g101", {"name": "새 이름"}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertFalse([s for s, _ in cur.q if "mcvideo" in s], "키가 없으면 MCVideo 표를 보지도 않는다")
        self.assertIn(("GROUP_CHANGED", "tel:g101", "PUT"), self.notified)

    def test_update_null_turns_off_object_turns_on(self):
        cur = self._db()
        r = _run(self.adm._update_group("g101", {"mcvideo": None}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(cur.sql("DELETE FROM mcvideo_group_attrs"), [("DELETE FROM mcvideo_group_attrs WHERE group_id=%s", (42,))])
        cur = self._db(attrs_row={"invite_members": 1, "max_duration_sec": 600, "max_transmitters": 2})
        r = _run(self.adm._update_group("g101", {"mcvideo": {"max_transmitters": 3}}, {}))
        self.assertEqual(r.status, 200, r.body)
        (sql, args), = cur.sql("INSERT INTO mcvideo_group_attrs")
        self.assertEqual((args[0], args[1], args[2], args[3]), (42, 1, 600, 3), "준 키만 바꾸고 나머지는 기존 행 값")

    def test_update_invalid_mcvideo_is_400_before_any_write(self):
        cur = self._db()
        r = _run(self.adm._update_group("g101", {"name": "x", "mcvideo": {"max_transmitters": 99}}, {}))
        self.assertEqual(r.status, 400)
        self.assertIn("max_transmitters", r.body["error"])
        self.assertEqual(cur.q, [], "검사는 DB 쓰기 전에 — autocommit 이라 부분 반영이 남는다")
        self.assertEqual(self.notified, [])

    def test_update_pre_migration(self):
        cur = self._db(tables=False)
        r = _run(self.adm._update_group("g101", {"name": "x", "mcvideo": {"invite_members": True}}, {}))
        self.assertEqual((r.status, r.body), (400, mv.SCHEMA_ERROR))
        self.assertFalse(cur.sql("UPDATE ptt_groups"), "표가 없으면 다른 필드도 쓰지 않는다")
        cur = self._db(tables=False)
        r = _run(self.adm._update_group("g101", {"mcvideo": None}, {}))
        self.assertEqual(r.status, 200, "표가 없으면 끄기는 할 일이 없다(MCVideo 그룹이 없다)")
        self.assertFalse(cur.sql("DELETE FROM mcvideo_group_attrs"))

    def test_create_with_mcvideo_writes_attrs_for_new_group(self):
        cur = self._db()
        r = _run(self.adm._create_group({"id": "g201", "group_type": "chat", "mcvideo": {"invite_members": False}}, {}))
        self.assertEqual(r.status, 201, r.body)
        (sql, args), = cur.sql("INSERT INTO mcvideo_group_attrs")
        self.assertEqual(args[0], 77, "새 그룹의 surrogate id(lastrowid)")
        self.assertEqual(args[3], mv.GROUP_ATTR_DEFAULTS["max_transmitters"], "빠진 키는 기본값")
        cur = self._db(tables=False)
        r = _run(self.adm._create_group({"id": "g202", "mcvideo": {}}, {}))
        self.assertEqual((r.status, r.body), (400, mv.SCHEMA_ERROR))
        self.assertFalse(cur.sql("INSERT INTO ptt_groups"))
        cur = self._db()
        r = _run(self.adm._create_group({"id": "g203"}, {}))
        self.assertEqual(r.status, 201)
        self.assertFalse(cur.sql("INSERT INTO mcvideo_group_attrs"), "mcvideo 없음 = MCVideo 그룹 아님")

    def test_ptt_line_entitlement_put_get_delete(self):
        line = "+82510002001"
        cur = self._db()
        r = _run(self.adm._put_ptt_mcvideo("7", line, {"max_video_streams": 2}, {}))
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(r.body, {"id": line, "max_video_streams": 2, "max_calls_n6": 1})
        (sql, args), = cur.sql("INSERT INTO mcvideo_user_profile")
        self.assertEqual(args, (line, 2, 1))
        self.assertEqual(mv.profile_of(line), {"max_video_streams": 2, "max_calls_n6": 1}, "캐시 = 문서·scope 가 곧바로 따른다")
        self.assertIn(("USER_CHANGED", f"tel:{line}", "PUT"), self.notified)
        cur = self._db(profile_row={"max_video_streams": 2, "max_calls_n6": 1})
        r = _run(self.adm._put_ptt_mcvideo("7", line, {"max_calls_n6": 3}, {}))
        self.assertEqual(r.body["max_video_streams"], 2, "준 키만 바꾼다")
        self.assertEqual(r.body["max_calls_n6"], 3)
        r = _run(self.adm._get_ptt_mcvideo("7", line, {}))
        self.assertEqual((r.status, r.body["max_video_streams"]), (200, 2))
        r = _run(self.adm._delete_ptt_mcvideo("7", line, {}))
        self.assertEqual(r.status, 200)
        self.assertIsNone(mv.profile_of(line))
        self.assertIn(("USER_CHANGED", f"tel:{line}", "DELETE"), self.notified)

    def test_ptt_line_entitlement_errors(self):
        line = "+82510002001"
        self._db()
        self.assertEqual(_run(self.adm._get_ptt_mcvideo("7", line, {})).body["error"], "not_entitled")
        self.assertEqual(_run(self.adm._delete_ptt_mcvideo("7", line, {})).status, 404)
        for bad in ({"max_video_streams": 0}, {"max_video_streams": 17}, {"max_calls_n6": True},
                    {"max_calls_n6": "2"}, {"bogus": 1}):
            with self.subTest(bad=bad):
                cur = self._db()
                self.assertEqual(_run(self.adm._put_ptt_mcvideo("7", line, bad, {})).status, 400)
                self.assertFalse(cur.sql("INSERT INTO mcvideo_user_profile"))
        self._db(owns_line=False)
        r = _run(self.adm._put_ptt_mcvideo("7", line, {"max_video_streams": 1}, {}))
        self.assertEqual((r.status, r.body["error"]), (404, "Subscription not found"))
        self._db(tables=False)
        r = _run(self.adm._put_ptt_mcvideo("7", line, {}, {}))
        self.assertEqual((r.status, r.body), (400, mv.SCHEMA_ERROR))

    def test_api_docs_declare_endpoints(self):
        ids = {d["id"] for d in self.adm.CIMS_ADMIN_API_DOCS}
        self.assertTrue({"csc.users.ptt.mcvideo.get", "csc.users.ptt.mcvideo.put", "csc.users.ptt.mcvideo.delete"} <= ids)


class CmsDocumentTest(unittest.TestCase):
    def setUp(self):
        self.keep = FIXTURE_SCENARIO()
        self._et = m.extract_token
        self.token = {"sub": "u1", "mcptt_id": A, "scope": " ".join(m.SCOPE_VIDEO_SERVICES)}
        m.extract_token = lambda hdr: self.token if hdr else None

    def tearDown(self):
        m.extract_token = self._et
        _restore(self.keep)

    def test_user_profile_lists_only_mcvideo_groups(self):
        root = ET.fromstring(mv.get_user_profile_xml(A)[0])
        gids = [e.text for e in root.findall("up:OnNetwork/up:MCVideoGroupInfo/up:MCVideo-Group-ID/up:uri-entry", NS)]
        self.assertEqual(gids, ["tel:g101"])
        self.assertEqual(root.find("up:OnNetwork/up:MaxSimultaneousVideoStreams", NS).text, "1")
        self.assertEqual(root.get("XUI-URI"), A)
        streams_c = ET.fromstring(mv.get_user_profile_xml(C)[0]).find("up:OnNetwork/up:MaxSimultaneousVideoStreams", NS)
        self.assertEqual(streams_c.text, "4")

    def test_user_profile_implicit_affiliations(self):
        """멤버 implicit_affiliation 이 켜진 MCVideo 그룹만 <ImplicitAffiliations> 에 — CSP 가 등록 때 MCVideo 제휴를 기록한다
        (TS 24.281 §8.2.2.2.15). 표시가 없으면 요소를 싣지 않는다(골든 그대로)."""
        self.assertIsNone(ET.fromstring(mv.get_user_profile_xml(A)[0]).find("up:OnNetwork/up:ImplicitAffiliations", NS))
        for g in m.GROUPS.values():
            for mb in g.get("members", []):
                if m._uri_eq(mb.get("uri"), A):
                    mb["implicit_affiliation"] = True
        on = ET.fromstring(mv.get_user_profile_xml(A)[0]).find("up:OnNetwork", NS)
        ids = [e.text for e in on.findall("up:ImplicitAffiliations/up:entry/up:uri-entry", NS)]
        self.assertEqual(ids, ["tel:g101"])               # 음성 전용 g102 는 MCVideo 그룹이 아니다
        tags = [c.tag.split("}")[1] for c in on]
        self.assertEqual(tags.index("ImplicitAffiliations"), tags.index("MaxAffiliationsN2") + 1)

    def test_no_profile_row_means_no_document(self):
        del mv.MCVIDEO_PROFILES[B[4:]]
        self.assertEqual(mv.get_user_profile_xml(B), (None, None))

    def test_service_config_follows_config_and_keeps_protection_false(self):
        mv.apply_config({"McVideoServiceConfig": {"TcTimersCounters": {"T11-stream-reception-idle": 20000,
                                                                       "C11-media-receivers": 8}}})
        root = ET.fromstring(mv.get_service_config_xml()[0])
        on = root.find("sc:service-configuration-params/sc:on-network", NS)
        self.assertEqual(on.find("sc:signalling-protection/sc:confidentiality-protection", NS).text, "false")
        self.assertEqual(on.find("sc:signalling-protection/sc:integrity-protection", NS).text, "false")
        tc = on.find("sc:anyExt/sc:tc-timers-counters-R14", NS)
        self.assertEqual(tc.find("sc:T11-stream-reception-idle", NS).text, "PT20S")
        self.assertEqual(tc.find("sc:C11-media-receivers", NS).text, "8")
        self.assertEqual(root.find("sc:service-configuration-params", NS).get("domain"), DOMAIN)

    def test_ue_init_config_mcvideo_details(self):
        root = ET.fromstring(m.get_ue_init_config_xml(BASE_URL)[0])
        ext = root.find("ue:on-network/ue:anyExt", NS)
        self.assertEqual([c.tag.split('}')[1] for c in ext],
                         ["MCPTT-Service-Details", "MCVideo-Service-Details", "MCData-Service-Details"])
        self.assertEqual(ext.find("ue:MCVideo-Service-Details/ue:Server-URI", NS).text, f"sip:mcvideo_psi@{DOMAIN}")
        m.UE_INIT_CONFIG = {}                         # 기본값 = MCVideo 광고 안 함(Roles.MCVIDEO 켠 사이트만 켠다)
        root = ET.fromstring(m.get_ue_init_config_xml(BASE_URL + "/x")[0])
        self.assertIsNone(root.find("ue:on-network/ue:anyExt/ue:MCVideo-Service-Details", NS))

    def _args(self, path, headers=None):
        h = {"authorization": "Bearer x"}
        h.update(headers or {})
        return HandlerArgs("GET", path, "127.0.0.1", 0, headers=h)

    def test_cms_handlers_authorise(self):
        up_path = f"/org.3gpp.mcvideo.user-profile/users/{A}/mcvideo-user-profile-1.xml"
        r = _run(m.handle_mcvideo_user_profile(self._args(up_path), {}))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.media_type, mv.MIME_USER_PROFILE)
        r2 = _run(m.handle_mcvideo_user_profile(self._args(up_path, {"if-none-match": r.headers["Etag"]}), {}))
        self.assertEqual(r2.status, 304)
        other = _run(m.handle_mcvideo_user_profile(
            self._args(f"/org.3gpp.mcvideo.user-profile/users/{B}/mcvideo-user-profile-1.xml"), {}))
        self.assertEqual(other.status, 403)
        sc = _run(m.handle_mcvideo_service_config(
            self._args("/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml"), {}))
        self.assertEqual(sc.status, 200)
        self.assertEqual(sc.media_type, mv.MIME_SERVICE_CONFIG)
        # MCPTT CMS scope 만 가진 토큰은 MCVideo 문서를 못 받는다(TS 33.180 B.4.2.2 — video_config_management_service)
        self.token = {"sub": "u1", "mcptt_id": A, "scope": m.SCOPE_PTT_CMS}
        self.assertEqual(_run(m.handle_mcvideo_user_profile(self._args(up_path), {})).status, 403)
        # 자격 행이 없으면 404
        self.token = {"sub": "u2", "mcptt_id": B, "scope": m.SCOPE_VIDEO_CMS}
        del mv.MCVIDEO_PROFILES[B[4:]]
        r404 = _run(m.handle_mcvideo_user_profile(
            self._args(f"/org.3gpp.mcvideo.user-profile/users/{B}/mcvideo-user-profile-1.xml"), {}))
        self.assertEqual(r404.status, 404)

    def test_token_carries_mcvideo_id_only_when_entitled(self):
        saved = m.storage
        class _S:
            def save_refresh_token(self, *a, **k):
                pass
        m.storage = _S()
        try:
            _i, acc, _r = m.create_tokens("u1", "3gpp:mc:video_service", mcptt_id=A)
            self.assertEqual(jwt.decode(acc, options={"verify_signature": False}).get("mcvideo_id"), A)
            del mv.MCVIDEO_PROFILES[B[4:]]
            _i, acc2, _r = m.create_tokens("u2", "3gpp:mc:ptt_service", mcptt_id=B)
            self.assertNotIn("mcvideo_id", jwt.decode(acc2, options={"verify_signature": False}))
        finally:
            m.storage = saved


class TcDefsAgreementTest(unittest.TestCase):
    """service configuration <tc-timers-counters-R14> 의 CSC 기본값 = 전송 제어 정의 정본(mcvideo_tc_defs.yaml, 계약 K5)의 기본값.
    R14 요소 17개는 XSD 상 전부 필수(TS 24.484 §9.4.2.3)라 CSC 는 늘 다 싣는다 — 값의 근원은 yaml 한 곳(server45_handoff §11 제안 1)."""

    def test_r14_defaults_match_k5_table(self):
        sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))
        from gen_floor_defs import load_table
        t = load_table(os.path.join(_REPO_ROOT, "docs", "design", "features", "mcvideo_tc_defs.yaml"))
        by_elem = {}
        for sec in ("participant_timers", "server_timers"):
            for k, v in t[sec].items():
                for key in ("element", "element_private"):
                    if v.get(key):
                        by_elem[v[key]] = ("ms", v["default_ms"])
        for k, v in t["server_counters"].items():
            by_elem[v["element"]] = ("n", v["default"])
        csc = mv._SERVICE_CONFIG_PARAM_DEFAULTS["TcTimersCounters"]
        self.assertEqual(len(csc), 17, "R14 요소 17개(TS 24.484 XSD)")
        for elem, val in csc.items():
            with self.subTest(element=elem):
                self.assertIn(elem, by_elem, "yaml 에 이 R14 요소가 없다")
                unit, dflt = by_elem[elem]
                if elem in mv._TC_UBYTE_SECONDS:
                    self.assertEqual(val * 1000, dflt)           # CSC = 초(xs:unsignedByte), yaml = ms
                else:
                    self.assertEqual(val, dflt)


def _write_fixtures():
    keep = FIXTURE_SCENARIO()
    try:
        os.makedirs(FIX, exist_ok=True)
        for name, gen in GOLDEN.items():
            with open(os.path.join(FIX, name), "w", encoding="utf-8") as f:
                f.write(gen().rstrip("\n") + "\n")
            print("wrote", os.path.relpath(os.path.join(FIX, name), _REPO_ROOT))
    finally:
        _restore(keep)


if __name__ == "__main__":
    if "--write-fixtures" in sys.argv:
        _write_fixtures()
    else:
        unittest.main()
