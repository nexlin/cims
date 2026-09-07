"""csc — 관제 앱 관리 평면 단위 시험 (오프라인, 가짜 DB·임시 ServiceLogDir).

dispatch_center.md §3.4·§5.6a · android_ue_provisioning.md §3-2/§3-3:
  - `dispatch_groups.directory_admin`(none|own|all) → 관리 범위(admin_scope)·범위 판정(in_scope)·조직 하위 집합
  - `/provisioning/directory/{admin,orgs,members,groups}` 게이트(401/403 no_directory_admin/out_of_scope)·라우팅
  - `/provisioning/history` 의 `until` 창 조회 + 항목 `recordingId`/`hasRecording`
  - `/provisioning/recordings/{id}` 의 id 검증·범위 판정(ptt 그룹 키·volte 당사자)·OAM 프록시 응답 변환
  - `/provisioning/me` dispatch 블록의 `directoryAdmin`/`orgCode`
  - GMS PUT/DELETE 의 관리 범위 확장(_admin_manages_group)

  python3 -m unittest tests.test_csc_dispatch_management
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import types
import unittest
from datetime import timedelta

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

import services.dispatch_history as dh  # noqa: E402
import services.mcptt as m  # noqa: E402
import handlers.dispatch as hd  # noqa: E402
import handlers.dispatch_directory as dd  # noqa: E402
import handlers.dispatch_recordings as dr  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402
from tests.test_csc_provisioning_history import _Tree  # noqa: E402

# organizations: (id, code, name, parent_id, sort)
ORGS = [(1, "CORP", "CIMS", None, 0), (2, "DIV1", "제1본부", 1, 1), (3, "TEAM01", "팀01", 2, 1),
        (4, "TEAM02", "팀02", 2, 2), (5, "DIV2", "제2본부", 1, 2)]


class ScopeTests(unittest.TestCase):
    def test_subtree_codes(self):
        self.assertEqual(dd.org_subtree_codes(ORGS, 2), {"DIV1", "TEAM01", "TEAM02"})
        self.assertEqual(dd.org_subtree_codes(ORGS, 1), {"CORP", "DIV1", "TEAM01", "TEAM02", "DIV2"})
        self.assertEqual(dd.org_subtree_codes(ORGS, 99), set())

    def test_subtree_cycle_safe(self):
        rows = ORGS + [(6, "LOOP", "loop", 6, 0)]
        self.assertEqual(dd.org_subtree_codes(rows, 6), {"LOOP"})

    def test_in_scope(self):
        own = {"orgCodes": {"DIV1", "TEAM01"}}
        self.assertTrue(dd.in_scope(own, "TEAM01"))
        self.assertFalse(dd.in_scope(own, "DIV2"))
        self.assertFalse(dd.in_scope(own, ""))
        self.assertTrue(dd.in_scope({"orgCodes": None}, ""))       # all — 무소속 구성원도 범위 안


class _DictCur:
    """dispatch_directory.admin_scope 가 내는 SQL 만 흉내 내는 DictCursor."""

    def __init__(self, group_row, has_col=True, has_tables=True):
        self.group_row = group_row
        self.has_col = has_col
        self.has_tables = has_tables
        self._rows = []

    def execute(self, q, args=None):
        self._rows = []
        if q.startswith("SHOW TABLES LIKE") or "information_schema" in q and "dispatch_groups" in q:
            self._rows = [{"x": 1}] if self.has_tables else []
        elif q.startswith("SHOW COLUMNS FROM dispatch_groups LIKE 'directory_admin'"):
            self._rows = [{"Field": "directory_admin"}] if self.has_col else []
        elif q.startswith("SELECT g.id, g.directory_admin, g.org_id"):
            self._rows = [self.group_row] if self.group_row else []
        elif q.startswith("SELECT id, code, name, parent_id, sort_order FROM organizations"):
            self._rows = [{"id": i, "code": c, "name": n, "parent_id": p, "sort_order": s} for i, c, n, p, s in ORGS]
        else:
            raise AssertionError("unexpected SQL: " + q)

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)


class AdminScopeTests(unittest.TestCase):
    def setUp(self):
        hd._HAS_TABLES = True
        hd._HAS_DIR_ADMIN = None

    def tearDown(self):
        hd._HAS_TABLES = None
        hd._HAS_DIR_ADMIN = None

    def test_none_is_no_scope(self):
        cur = _DictCur({"id": "dg-1", "directory_admin": "none", "org_id": 2})
        self.assertIsNone(dd.admin_scope(cur, 5020))

    def test_own_is_org_subtree(self):
        cur = _DictCur({"id": "dg-1", "directory_admin": "own", "org_id": 2})
        sc = dd.admin_scope(cur, 5020)
        self.assertEqual((sc["groupId"], sc["directoryAdmin"], sc["orgCode"]), ("dg-1", "own", "DIV1"))
        self.assertEqual(sc["orgCodes"], {"DIV1", "TEAM01", "TEAM02"})

    def test_own_without_org_is_no_scope(self):
        cur = _DictCur({"id": "dg-1", "directory_admin": "own", "org_id": None})
        self.assertIsNone(dd.admin_scope(cur, 5020))

    def test_all_is_unbounded(self):
        cur = _DictCur({"id": "dg-1", "directory_admin": "all", "org_id": None})
        sc = dd.admin_scope(cur, 5020)
        self.assertEqual(sc["directoryAdmin"], "all")
        self.assertIsNone(sc["orgCodes"])

    def test_column_missing_is_no_scope(self):
        cur = _DictCur({"id": "dg-1", "directory_admin": "all", "org_id": 1}, has_col=False)
        self.assertIsNone(dd.admin_scope(cur, 5020))

    def test_non_member_is_no_scope(self):
        self.assertIsNone(dd.admin_scope(_DictCur(None), 5020))
        self.assertIsNone(dd.admin_scope(_DictCur({"id": "dg-1", "directory_admin": "all", "org_id": 1}), None))


class RoutingTests(unittest.TestCase):
    """핸들러 진입 게이트 — 토큰·scope·관리 범위. DB 는 admin_scope 스텁."""

    def setUp(self):
        self._saved = (m.extract_token, dd.admin_scope, dd.caller_identity, dd._get_db)
        self.token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
        m.extract_token = lambda hdr: self.token if hdr else None
        dd.caller_identity = lambda cur, tok: ("+821310001001", 5020)
        self.scope = {"groupId": "dg-1", "directoryAdmin": "own", "orgCode": "DIV1", "orgCodes": {"DIV1", "TEAM01"}}
        dd.admin_scope = lambda cur, uid: self.scope
        cur = types.SimpleNamespace(execute=lambda q, a=None: None, fetchone=lambda: None, fetchall=lambda: [])
        dd._get_db = lambda cfg: _Conn(cur)

    def tearDown(self):
        m.extract_token, dd.admin_scope, dd.caller_identity, dd._get_db = self._saved

    def _call(self, method, path, body=None, token="x", headers=None):
        h = dict(headers or {})
        if token:
            h["authorization"] = "Bearer " + token
        a = HandlerArgs(method, path, "127.0.0.1", 0, headers=h, body=body)
        return asyncio.run(dd.handle_directory_admin(a, {"config": {}}))

    def test_no_token_401(self):
        self.assertEqual(self._call("GET", "/provisioning/directory/admin", token="").status, 401)

    def test_wrong_scope_403(self):
        self.token["scope"] = [m.SCOPE_PTT_SERVICE]
        r = self._call("GET", "/provisioning/directory/admin")
        self.assertEqual((r.status, r.body["error"]), (403, "insufficient_scope"))

    def test_no_directory_admin_403(self):
        self.scope = None
        r = self._call("GET", "/provisioning/directory/admin")
        self.assertEqual((r.status, r.body["error"]), (403, "no_directory_admin"))

    def test_admin_view_is_get_only(self):
        self.assertEqual(self._call("POST", "/provisioning/directory/admin", {}).status, 405)

    def test_unknown_path_404(self):
        self.assertEqual(self._call("GET", "/provisioning/directory/bogus").status, 404)

    def test_orgs_method_shape(self):
        self.assertEqual(self._call("POST", "/provisioning/directory/orgs/TEAM01", {}).status, 405)
        self.assertEqual(self._call("PUT", "/provisioning/directory/orgs", {}).status, 405)
        self.assertEqual(self._call("PUT", "/provisioning/directory/orgs/a/b", {}).status, 405)

    def test_routes_registered_on_mcptt_server(self):
        paths = {p for p, _h, _k in dd.CSC_DIRECTORY_ADMIN_HANDLER_LIST}
        self.assertEqual(paths, {"/provisioning/directory/admin", "/provisioning/directory/orgs",
                                 "/provisioning/directory/members", "/provisioning/directory/groups"})
        self.assertEqual([p for p, _h, _k in dr.CSC_RECORDINGS_HANDLER_LIST], ["/provisioning/recordings"])


class _Ctx:
    def __init__(self, obj):
        self.obj = obj

    def __enter__(self):
        return self.obj

    def __exit__(self, *a):
        return False


class _Conn:
    """pymysql Connection 흉내 — with conn / conn.cursor() 컨텍스트."""

    def __init__(self, cur):
        self.cur = cur

    def cursor(self):
        return _Ctx(self.cur)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def close(self):
        pass


class OrgWriteTests(unittest.TestCase):
    """_org_write — 범위·트리 무결성 검증(가짜 조직 테이블)."""

    class _Cur:
        def __init__(self):
            self._rows = []
            self.users = {"TEAM02": 1}

        def execute(self, q, args=None):
            self._rows = []
            if q.startswith("SELECT id, code, name, parent_id, sort_order FROM organizations WHERE code="):
                self._rows = [{"id": i, "code": c, "name": n, "parent_id": p, "sort_order": s}
                              for i, c, n, p, s in ORGS if c == args[0]]
            elif q.startswith("SELECT id, code, name, parent_id, sort_order FROM organizations"):
                self._rows = [{"id": i, "code": c, "name": n, "parent_id": p, "sort_order": s} for i, c, n, p, s in ORGS]
            elif q.startswith("SELECT COUNT(*) AS n FROM organizations WHERE parent_id="):
                self._rows = [{"n": sum(1 for r in ORGS if r[3] == args[0])}]
            elif q.startswith("SELECT COUNT(*) AS n FROM users WHERE org_id="):
                self._rows = [{"n": self.users.get(args[0], 0)}]
            else:
                raise AssertionError(q)

        def fetchone(self):
            return self._rows[0] if self._rows else None

        def fetchall(self):
            return list(self._rows)

    def setUp(self):
        self.cur = self._Cur()
        self.own = {"groupId": "dg-1", "directoryAdmin": "own", "orgCode": "DIV1", "orgCodes": {"DIV1", "TEAM01", "TEAM02"}}
        self.all = {"groupId": "dg-1", "directoryAdmin": "all", "orgCode": "", "orgCodes": None}

    def test_create_needs_parent_in_scope(self):
        r = dd._org_write(self.cur, {}, self.own, "POST", "", {"code": "X", "name": "x", "parent": "DIV2"}, "a", "")
        self.assertEqual((r.status, r.body["error"]), (403, "out_of_scope"))
        r = dd._org_write(self.cur, {}, self.own, "POST", "", {"code": "X", "name": "x"}, "a", "")
        self.assertEqual(r.status, 403)                                   # own 은 루트 신설 불가
        ok = dd._org_write(self.cur, {}, self.own, "POST", "", {"code": "X", "name": "x", "parent": "TEAM01", "sort": 3}, "a", "")
        self.assertEqual(ok[1], {"code": "X", "name": "x", "parent_id": 3, "sort_order": 3})
        ok = dd._org_write(self.cur, {}, self.all, "POST", "", {"code": "R", "name": "r"}, "a", "")
        self.assertIsNone(ok[1]["parent_id"])                              # all 은 루트 허용

    def test_create_conflicts(self):
        r = dd._org_write(self.cur, {}, self.all, "POST", "", {"code": "TEAM01", "name": "dup"}, "a", "")
        self.assertEqual((r.status, r.body["error"]), (409, "code_exists"))
        r = dd._org_write(self.cur, {}, self.all, "POST", "", {"code": "N", "name": "n", "parent": "NOPE"}, "a", "")
        self.assertEqual((r.status, r.body["error"]), (400, "unknown_parent"))

    def test_update_scope_and_cycle(self):
        r = dd._org_write(self.cur, {}, self.own, "PUT", "DIV2", {"name": "x"}, "a", "")
        self.assertEqual(r.status, 403)
        r = dd._org_write(self.cur, {}, self.own, "PUT", "DIV1", {"parent": "TEAM01"}, "a", "")
        self.assertEqual(r.status, 403)                                   # 범위 루트 이동 불가
        r = dd._org_write(self.cur, {}, self.all, "PUT", "DIV1", {"parent": "TEAM01"}, "a", "")
        self.assertEqual((r.status, r.body["error"]), (400, "cyclic_parent"))
        ok = dd._org_write(self.cur, {}, self.own, "PUT", "TEAM01", {"name": "팀1", "sort": 9}, "a", "")
        self.assertEqual(ok[1], (3, {"name": "팀1", "sort_order": 9}))

    def test_delete_not_empty(self):
        r = dd._org_write(self.cur, {}, self.own, "DELETE", "DIV1", None, "a", "")
        self.assertEqual(r.status, 403)                                   # 범위 루트 삭제 불가
        r = dd._org_write(self.cur, {}, self.all, "DELETE", "DIV1", None, "a", "")
        self.assertEqual((r.status, r.body["error"]), (409, "not_empty"))  # 하위 조직
        r = dd._org_write(self.cur, {}, self.own, "DELETE", "TEAM02", None, "a", "")
        self.assertEqual((r.status, r.body["error"]), (409, "not_empty"))  # 구성원
        ok = dd._org_write(self.cur, {}, self.own, "DELETE", "TEAM01", None, "a", "")
        self.assertEqual(ok[1], 3)
        self.assertEqual(dd._org_write(self.cur, {}, self.all, "DELETE", "NOPE", None, "a", "").status, 404)


class SubBodyTests(unittest.TestCase):
    def test_imsi_defaults_to_number_digits(self):
        b = dd._sub_body("volte", {"msisdn": "+821310001001", "serviceRef": "volte", "sipTransport": "tls", "password": "p"})
        self.assertEqual(b, {"id": "+821310001001", "imsi": "821310001001", "service_ref": "volte",
                             "sip_transport": "TLS", "passwd": "p"})
        self.assertEqual(dd._sub_body("ptt", {"msisdn": "+8250", "imsi": "45033"})["imsi"], "45033")


class HistoryWindowTests(unittest.TestCase):
    def setUp(self):
        self.t = _Tree()

    def _scope(self, members=(), ptt=()):
        return {"members": {dh.userpart(x) for x in members}, "ptt_groups": set(ptt),
                "groupId": "dg1", "monitorScope": "all", "pttListen": "all"}

    def test_until_window(self):
        self.t.group_msg("g002", "+82510002001", "old", minutes_ago=40, msg_id="o")
        self.t.group_msg("g002", "+82510002001", "mid", minutes_ago=20, msg_id="m")
        self.t.group_msg("g002", "+82510002001", "new", minutes_ago=2, msg_id="n")
        since = self.t.now - timedelta(minutes=30)
        until = self.t.now - timedelta(minutes=10)
        items, _ = dh.query(self.t.sl, "message", self._scope(ptt=["g002"]), since, 100, until)
        self.assertEqual([x["text"] for x in items], ["mid"])

    def test_recording_id_and_flag(self):
        self.t.ptt("g002", "ses-1", "+82510002001")
        self.t.call("call-A", "+821310002001", "+821310009999")
        items, _ = dh.query(self.t.sl, "ptt", self._scope(ptt=["g002"]), None, 10)
        p = items[0]
        self.assertTrue(p["recordingId"].startswith("ptt/1/"))
        self.assertRegex(p["recordingId"], r'^ptt/1/\d{4}/\d{2}/\d{2}/\d{2}/S1$')
        self.assertFalse(p["hasRecording"])
        # segments.jsonl 이 생기면 녹취 있음
        with open(os.path.join(self.t.sl, p["recordingId"], "segments.jsonl"), "w") as f:
            f.write("{}\n")
        items, _ = dh.query(self.t.sl, "ptt", self._scope(ptt=["g002"]), None, 10)
        self.assertTrue(items[0]["hasRecording"])
        w = dh.format_item(items[0])
        self.assertEqual(w["recordingId"], p["recordingId"]); self.assertTrue(w["hasRecording"])
        c = dh.query(self.t.sl, "call", self._scope(members=["+821310002001"]), None, 10)[0][0]
        self.assertRegex(c["recordingId"], r'^volte/\d{4}/\d{2}/\d{2}/\d{2}/.+/call-A\.d$')
        self.assertEqual(dh.format_item({"kind": "call", "ts": "t", "id": "x"})["recordingId"], "")


class RecordingsTests(unittest.TestCase):
    def test_parts(self):
        self.assertEqual(dr._parts("/provisioning/recordings/ptt/24/2026/09/07/10/S1_1"),
                         ("ptt/24/2026/09/07/10/S1_1", None, None))
        self.assertEqual(dr._parts("/provisioning/recordings/ptt/24/2026/09/07/10/S1_1/segments/3/audio?slot=1"),
                         ("ptt/24/2026/09/07/10/S1_1", "3", "audio"))
        self.assertEqual(dr._parts("/provisioning/recordings/volte/2026/09/07/10/010/0100/c1.d/segments/1/peaks"),
                         ("volte/2026/09/07/10/010/0100/c1.d", "1", "peaks"))
        self.assertEqual(dr._parts("/provisioning/other"), (None, None, None))

    def test_safe_id(self):
        self.assertTrue(dr._safe_id("ptt/24/2026/09/07/10/S1_1"))
        self.assertFalse(dr._safe_id("../etc"))
        self.assertFalse(dr._safe_id("ptt/../../x"))
        self.assertFalse(dr._safe_id("/abs"))
        self.assertFalse(dr._safe_id(""))

    def test_in_scope_by_group_key_and_call_parties(self):
        t = _Tree()
        t.ptt("g002", "ses-1", "+82510002001")
        t.call("call-A", "+821310002001", "+821310009999")
        scope = {"members": {"+821310002001"}, "ptt_groups": {"g002"}}
        rec_ptt = dh.query(t.sl, "ptt", dict(scope, groupId="dg1"), None, 10)[0][0]["recordingId"]
        rec_call = dh.query(t.sl, "call", dict(scope, groupId="dg1"), None, 10)[0][0]["recordingId"]
        self.assertTrue(dr.in_scope(t.sl, rec_ptt, scope, {"1": "g002"}))       # surrogate → mcptt id
        self.assertTrue(dr.in_scope(t.sl, rec_ptt, scope, {}))                   # session.json 대조 폴백
        self.assertFalse(dr.in_scope(t.sl, rec_ptt, {"members": set(), "ptt_groups": {"g009"}}, {"1": "g002"}))
        self.assertTrue(dr.in_scope(t.sl, rec_call, scope, {}))
        self.assertFalse(dr.in_scope(t.sl, rec_call, {"members": {"+8213107777"}, "ptt_groups": set()}, {}))
        self.assertFalse(dr.in_scope(t.sl, "message/x", scope, {}))

    def test_handler_gate_and_proxy(self):
        t = _Tree()
        t.ptt("g002", "ses-1", "+82510002001")
        rec = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}, "groupId": "dg1"}, None, 10)[0][0]["recordingId"]
        import services as _svc_pkg
        saved = (m.extract_token, m._SERVICE_LOG_DIR, dr._scope_sets, dr._http,
                 sys.modules.get("services.fm_reporter"), getattr(_svc_pkg, "fm_reporter", None))
        # 감사 발신 스텁 — 실제 fm_reporter 를 적재하면 다른 시험의 sys.modules 스텁이 무력화된다(패키지 속성 우선).
        audits = []
        fake_fm = types.SimpleNamespace(get=lambda: types.SimpleNamespace(node="n1", send_event=lambda *a, **k: audits.append(k)))
        sys.modules["services.fm_reporter"] = fake_fm
        _svc_pkg.fm_reporter = fake_fm
        try:
            token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
            m.extract_token = lambda hdr: token if hdr else None
            m._SERVICE_LOG_DIR = t.sl
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"groupId": "dg1", "members": set(), "ptt_groups": {"g002"}}, {"1": "g002"})
            calls = []

            class _Resp:
                def __init__(self, status, ct, content):
                    self.status_code, self.headers, self.content = status, {"content-type": ct}, content

            class _Http:
                def get(self, url, params=None, headers=None, timeout=None):
                    calls.append((url, params))
                    if url.endswith("/audio"):
                        return _Resp(200, "audio/mp4", b"\x00\x00\x00\x18ftyp")
                    return _Resp(200, "application/json", json.dumps({"id": rec, "segments": []}).encode())
            dr._http = lambda cfg: _Http()

            def call(path, token_hdr="Bearer x"):
                a = HandlerArgs("GET", path, "127.0.0.1", 0, headers={"authorization": token_hdr} if token_hdr else {},
                                query_params={"slot": "1"})
                return asyncio.run(dr.handle_recordings(a, {"config": {"Fm": {"OamIp": "10.0.0.1"}}}))

            self.assertEqual(call("/provisioning/recordings/" + rec, token_hdr="").status, 401)
            self.assertEqual(call("/provisioning/recordings/../x").status, 400)
            self.assertEqual(call("/provisioning/recordings/ptt/1/2026/01/01/00/nope").status, 404)
            r = call("/provisioning/recordings/" + rec)
            self.assertEqual(r.status, 200); self.assertEqual(r.body["id"], rec)
            self.assertTrue(calls[-1][0].startswith("https://10.0.0.1:4419/api/v1/recordings/ptt/1/"))
            r = call("/provisioning/recordings/" + rec + "/segments/3/audio")
            self.assertEqual((r.status, r.media_type), (200, "audio/mp4"))
            self.assertEqual(r.body[:4], b"\x00\x00\x00\x18")
            self.assertTrue(calls[-1][0].endswith("/segments/3/audio")); self.assertEqual(calls[-1][1], {"slot": "1"})
            # 범위 밖
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"groupId": "dg1", "members": set(), "ptt_groups": {"g009"}}, {})
            self.assertEqual(call("/provisioning/recordings/" + rec).body["error"], "out_of_scope")
            dr._scope_sets = lambda cfg, tok: ("+821310001001", None, {})
            self.assertEqual(call("/provisioning/recordings/" + rec).body["error"], "no_monitor_scope")
            # 감사 E-AUD-016 tap_mode=recording — 오디오 200 한 번
            self.assertEqual([a["params"]["tap_mode"] for a in audits], ["recording"])
        finally:
            m.extract_token, m._SERVICE_LOG_DIR, dr._scope_sets, dr._http, fm_mod, fm_attr = saved
            if fm_mod is None:
                sys.modules.pop("services.fm_reporter", None)
            else:
                sys.modules["services.fm_reporter"] = fm_mod
            if fm_attr is None:
                if hasattr(_svc_pkg, "fm_reporter"):
                    delattr(_svc_pkg, "fm_reporter")
            else:
                _svc_pkg.fm_reporter = fm_attr

    def test_verify_tls_string_config(self):
        dr._client = None
        self.assertFalse(dr._http({"Recording": {"VerifyTls": "false"}}).verify)
        dr._client = None
        self.assertTrue(dr._http({"Recording": {"VerifyTls": "true"}}).verify)
        dr._client = None
        self.assertFalse(dr._http({}).verify)
        dr._client = None

    def test_oam_base_config(self):
        self.assertEqual(dr._oam_base({"Recording": {"OamUrl": "https://vip:4419/"}}), "https://vip:4419")
        self.assertEqual(dr._oam_base({"Fm": {"OamIp": "10.1.1.1"}}), "https://10.1.1.1:4419")


class GmsAdminGateTests(unittest.TestCase):
    """_admin_manages_group — 소유자가 아니어도 관리 범위 안 그룹은 관리, 범위 밖·범위 없음은 거부."""

    def setUp(self):
        self._saved = (m._db_connect, dd.caller_identity, dd.admin_scope)
        m._db_connect = lambda: _Conn(types.SimpleNamespace())
        dd.caller_identity = lambda c, tok: ("+821310001001", 5020)

    def tearDown(self):
        m._db_connect, dd.caller_identity, dd.admin_scope = self._saved

    def test_scope_decides(self):
        dd.admin_scope = lambda c, uid: {"orgCodes": {"TEAM01"}}
        self.assertTrue(m._admin_manages_group({}, {"org_code": "TEAM01"}))
        self.assertFalse(m._admin_manages_group({}, {"org_code": "TEAM02"}))
        self.assertFalse(m._admin_manages_group({}, {"org_code": ""}))
        self.assertTrue(m._admin_manages_group({}, None))                     # 신규 생성
        dd.admin_scope = lambda c, uid: None
        self.assertFalse(m._admin_manages_group({}, {"org_code": "TEAM01"}))
        self.assertFalse(m._admin_manages_group({}, None))


class ServiceCatalogTests(unittest.TestCase):
    """접속서비스 후보(`services.<kind>[].name`) — CSC 는 CSP access_services 컬렉션을 못 읽으므로(관리 store 가
    다르다) csc.json Provisioning.Services.<kind> 가 정본. `name` 이 service_ref 후보가 된다(없으면 kind)."""

    def setUp(self):
        import services.file_store as fs
        self._fs_load_all = fs.load_all
        fs.load_all = lambda d: []          # 미러 비어 있음(개발 서버·CSC 표준 배포 공통)

    def tearDown(self):
        import services.file_store as fs
        fs.load_all = self._fs_load_all

    def test_fallback_uses_configured_name(self):
        cfg = {"Provisioning": {"Services": {
            "volte": {"name": "volte", "domain": "ims.example"},
            "ptt": {"name": "mcptt", "domain": "ptt.example"}}}}
        out = dd._services(cfg)
        self.assertEqual(out["volte"], [{"name": "volte", "domain": "ims.example"}])
        self.assertEqual(out["ptt"], [{"name": "mcptt", "domain": "ptt.example"}])

    def test_fallback_without_name_uses_kind(self):
        cfg = {"Provisioning": {"Services": {"ptt": {"domain": "ptt.example"}}}}
        self.assertEqual(dd._services(cfg)["ptt"], [{"name": "ptt", "domain": "ptt.example"}])


if __name__ == "__main__":
    unittest.main()


class MemberWriteTests(unittest.TestCase):
    """_member_write — 번호 변경 시 종전 접속서비스·transport 승계, PTT 프로파일 PUT 의 감사 페이로드(선택 컬럼 부재)."""

    class _Cur:
        def __init__(self):
            self._rows = []

        def execute(self, q, args=None):
            self._rows = []
            if q.startswith("SELECT id, org_id, name FROM users WHERE id="):
                self._rows = [{"id": 6000, "org_id": "TEAM01", "name": "시험"}]
            elif "FROM volte_subscriptions WHERE user_id=" in q:
                self._rows = [{"id": "+821310009901", "service_ref": "volte", "sip_transport": "TLS"}]
            elif "FROM ptt_subscriptions WHERE user_id=" in q:
                self._rows = [{"id": "+82510009901", "service_ref": "mcptt", "sip_transport": "TLS"}]
            elif "WHERE id=%s" in q and "FROM volte_subscriptions" in q:
                self._rows = []                                   # 새 번호는 아무도 안 씀
            else:
                raise AssertionError(q)

        def fetchone(self):
            return self._rows[0] if self._rows else None

        def fetchall(self):
            return list(self._rows)

    def setUp(self):
        self.calls = []
        self._saved = (dd._admin._add_subscription, dd._admin._delete_subscription, dd._admin._put_ptt_profile, dd._audit, m.get_user_profile)

        async def add(uid, svc, body, cfg): self.calls.append(("add", uid, svc, dict(body))); return dd.HandlerResult(status=201, body={"id": body["id"]})
        async def dele(uid, svc, ms, cfg): self.calls.append(("del", uid, svc, ms)); return dd.HandlerResult(status=200, body={"id": ms})
        async def prof(uid, ms, body, cfg): self.calls.append(("prof", uid, ms, dict(body))); return dd.HandlerResult(status=200, body=dict(body))
        dd._admin._add_subscription, dd._admin._delete_subscription, dd._admin._put_ptt_profile = add, dele, prof
        dd._audit = lambda *a, **k: self.calls.append(("audit", a[3], a[5], k.get("after")))
        m.get_user_profile = lambda ms: {"allow_emergency_call": True, "allow_emergency_alert": True, "allow_adhoc_call": True,
                                        "allow_emergency_private_call": True, "allow_ambient_listening": False, "allow_create_group": False}
        self.scope = {"groupId": "dg-1", "directoryAdmin": "own", "orgCode": "TEAM01", "orgCodes": {"TEAM01"}}

    def tearDown(self):
        dd._admin._add_subscription, dd._admin._delete_subscription, dd._admin._put_ptt_profile, dd._audit, m.get_user_profile = self._saved

    def _run(self, method, parts, body):
        return asyncio.run(dd._member_write(self._Cur(), {}, self.scope, method, parts, body, "+8213", "1.2.3.4", 5020))

    def test_number_change_inherits_service_and_transport(self):
        r = self._run("PUT", ("6000", "volte"), {"msisdn": "+821310009902", "password": "1234"})
        self.assertEqual(r.status, 201)
        kinds = [c[0] for c in self.calls if c[0] in ("del", "add")]
        self.assertEqual(kinds, ["del", "add"])
        added = next(c for c in self.calls if c[0] == "add")[3]
        self.assertEqual((added["service_ref"], added["sip_transport"], added["passwd"]), ("volte", "TLS", "1234"))

    def test_number_change_requires_password(self):
        r = self._run("PUT", ("6000", "volte"), {"msisdn": "+821310009902"})
        self.assertEqual(r.status, 400)
        self.assertFalse(any(c[0] in ("del", "add") for c in self.calls))

    def test_profile_put_partial_keys_audit(self):
        r = self._run("PUT", ("6000", "ptt", "profile"), {"allowCreateGroup": True})
        self.assertEqual(r.status, 200, getattr(r, "body", None))
        prof = next(c for c in self.calls if c[0] == "prof")[3]
        self.assertEqual(prof.get("allow_create_group"), True)
        self.assertNotIn("allow_ambient_listening", prof)        # 현재 False 인 선택 컬럼은 싣지 않는다(미적용 DB 400 회피)
        self.assertTrue(prof.get("allow_emergency_call"))
        audit = next(c for c in self.calls if c[0] == "audit" and c[1] == "ptt_profile")
        self.assertEqual(audit[3]["allowAmbientListening"], False)
        self.assertEqual(audit[3]["allowCreateGroup"], True)
