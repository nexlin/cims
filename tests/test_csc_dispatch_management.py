"""csc — 관제 앱 관리 평면 단위 시험 (오프라인, 가짜 DB·임시 ServiceLogDir).

dispatch_center.md §3.4·§5.7a/b · mcptt_authorization.md §2 · android_ue_provisioning.md §3-2/§3-3:
  - 역할 `directory_write`(none|own|all) + `org_id` → 관리 범위(admin_scope)·범위 판정(in_scope)·조직 하위 집합
  - `/provisioning/directory/{admin,orgs,members,groups,phone-groups}` 게이트(401/403 no_directory_admin/out_of_scope)·라우팅
  - `/provisioning/history` 의 `until` 창 조회 + 항목 `recordingId`/`hasRecording`
  - `/provisioning/recordings/{id}` 의 id 검증·범위 판정(ptt 그룹 키·volte 당사자)·OAM 프록시 응답 변환
  - GMS PUT/DELETE 의 관리 범위 확장(_admin_manages_group = authz.can(ptt_group.manage))

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
import services.authz as az  # noqa: E402
import handlers.dispatch as hd  # noqa: E402
import handlers.dispatch_directory as dd  # noqa: E402
import handlers.dispatch_recordings as dr  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402
from tests.test_csc_provisioning_history import _Tree, _now_parts  # noqa: E402

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


def _role(dw="none", org_id=None, **kw):
    r = {'id': 'role-1', 'name': '관리', 'builtin': 0, 'authz_manage': 0, 'audit_read': 0, 'directory_write': dw,
         'directory_read': 'none', 'ptt_group_manage': 'none', 'monitor_call': 'none', 'ptt_listen': 'none',
         'listen_visibility': 'hidden', 'history_read': 'none', 'alarm_ack': 0, 'mcptt_control': 0, 'org_id': org_id}
    r.update(kw)
    return r


class _DictCur:
    """dispatch_directory.admin_scope(services/authz 역할 해석 + 전화 그룹 멤버십)가 내는 SQL 만 흉내 내는 DictCursor."""

    def __init__(self, role, assigned=True, has_tables=True, group="pg-1"):
        self.role = role
        self.assigned = assigned
        self.has_tables = has_tables
        self.group = group
        self._rows = []

    def execute(self, q, args=None):
        self._rows = []
        if q.startswith("SHOW TABLES LIKE"):
            self._rows = [{"x": 1}] if self.has_tables else []
        elif q.startswith("SELECT role_id FROM role_assignments WHERE principal_type='user'"):
            self._rows = [{"role_id": "role-1"}] if (self.role and self.assigned) else []
        elif q.startswith("SELECT " + ", ".join(az.ROLE_COLS) + " FROM roles WHERE id="):
            self._rows = [dict(self.role)] if self.role else []
        elif q.startswith("SELECT id, code, name, parent_id, sort_order FROM organizations"):
            self._rows = [{"id": i, "code": c, "name": n, "parent_id": p, "sort_order": s} for i, c, n, p, s in ORGS]
        elif q.startswith("SELECT m.group_id FROM phone_group_members m WHERE m.user_id IN ("):
            self._rows = [{"group_id": self.group}] if self.group else []
        else:
            raise AssertionError("unexpected SQL: " + q)

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)


class AdminScopeTests(unittest.TestCase):
    def setUp(self):
        hd._HAS_TABLES = None
        az.reset_probe()

    def tearDown(self):
        hd._HAS_TABLES = None
        az.reset_probe()

    def test_none_is_no_scope(self):
        self.assertIsNone(dd.admin_scope(_DictCur(_role("none", 2)), 5020))

    def test_own_is_org_subtree(self):
        sc = dd.admin_scope(_DictCur(_role("own", 2)), 5020)
        self.assertEqual((sc["roleId"], sc["groupId"], sc["directoryWrite"], sc["directoryAdmin"], sc["orgCode"]),
                         ("role-1", "pg-1", "own", "own", "DIV1"))
        self.assertEqual(sc["orgCodes"], {"DIV1", "TEAM01", "TEAM02"})
        self.assertEqual(sc["role"]["id"], "role-1")

    def test_own_without_org_is_no_scope(self):
        self.assertIsNone(dd.admin_scope(_DictCur(_role("own", None)), 5020))

    def test_all_is_unbounded(self):
        sc = dd.admin_scope(_DictCur(_role("all"), group=None), 5020)
        self.assertEqual((sc["directoryAdmin"], sc["groupId"]), ("all", ""))
        self.assertIsNone(sc["orgCodes"])

    def test_tables_missing_is_no_scope(self):
        self.assertIsNone(dd.admin_scope(_DictCur(_role("all", 1), has_tables=False), 5020))

    def test_unassigned_is_no_scope(self):
        self.assertIsNone(dd.admin_scope(_DictCur(_role("all", 1), assigned=False), 5020))
        self.assertIsNone(dd.admin_scope(_DictCur(None), 5020))
        self.assertIsNone(dd.admin_scope(_DictCur(_role("all", 1)), None))

    def test_in_scope_follows_authz(self):
        self.assertTrue(dd.in_scope({"orgCodes": None}, "ANY"))
        self.assertFalse(dd.in_scope({"orgCodes": {"A"}}, "B"))


class RoutingTests(unittest.TestCase):
    """핸들러 진입 게이트 — 토큰·scope·관리 범위. DB 는 admin_scope 스텁."""

    def setUp(self):
        self._saved = (m.extract_token, dd.admin_scope, dd.caller_identity, dd._get_db)
        self.token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
        m.extract_token = lambda hdr: self.token if hdr else None
        dd.caller_identity = lambda cur, tok: ("+821310001001", 5020)
        self.scope = {"roleId": "role-1", "groupId": "pg-1", "directoryWrite": "own", "directoryAdmin": "own",
                      "orgCode": "DIV1", "orgCodes": {"DIV1", "TEAM01"}, "role": _role("own", 2)}
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
                                 "/provisioning/directory/members", "/provisioning/directory/groups",
                                 "/provisioning/directory/phone-groups"})
        self.assertEqual([p for p, _h, _k in dr.CSC_RECORDINGS_HANDLER_LIST],
                         ["/provisioning/recordings", "/provisioning/history/ptt"])

    def test_phone_groups_share_console_write_code_and_audit(self):
        """/provisioning/directory/phone-groups → handlers.dispatch.dispatch_phone_group(같은 코드) + 범위 orgCodes + 감사 actor user:<id>."""
        calls, audits = [], []
        saved = (dd._dispatch.dispatch_phone_group, dd._audit)
        dd._dispatch.dispatch_phone_group = lambda cur, method, parts, body, codes, org_filter=None: (
            calls.append((method, parts, body, codes)) or dd.HandlerResult(status=201 if method == "POST" else 200,
                                                                            body={"id": "pg-9", "groups": []}))
        dd._audit = lambda cfg, actor, ip, entity, eid, action, after=None, **k: audits.append((actor, entity, eid, action))
        try:
            r = self._call("GET", "/provisioning/directory/phone-groups")
            self.assertEqual((r.status, calls[-1][0], calls[-1][1], calls[-1][3]), (200, "GET", (), {"DIV1", "TEAM01"}))
            self.assertEqual(audits, [])
            r = self._call("POST", "/provisioning/directory/phone-groups", {"name": "x", "org_id": 3})
            self.assertEqual((r.status, r.body["id"]), (201, "pg-9"))
            self.assertEqual(audits[-1], ("user:5020", "phone_group", "pg-9", "create"))
            self._call("POST", "/provisioning/directory/phone-groups/pg-9/members", {"user_id": "+8213"})
            self.assertEqual((calls[-1][1], audits[-1][3]), (("pg-9", "members"), "member_add"))
            self._call("DELETE", "/provisioning/directory/phone-groups/pg-9/members/%2B8213")
            self.assertEqual((calls[-1][1], audits[-1][3]), (("pg-9", "members", "+8213"), "member_remove"))
        finally:
            dd._dispatch.dispatch_phone_group, dd._audit = saved

    def test_members_import_route(self):
        saved = dd._members_import

        async def fake(cur, config, scope, body, actor, ip, my_uid):
            return dd._json(200, {"actor": actor})
        dd._members_import = fake
        try:
            r = self._call("POST", "/provisioning/directory/members/import", {"rows": []})
            self.assertEqual((r.status, r.body["actor"]), (200, "user:5020"))
        finally:
            dd._members_import = saved


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
        self.own = {"groupId": "pg-1", "directoryAdmin": "own", "orgCode": "DIV1", "orgCodes": {"DIV1", "TEAM01", "TEAM02"}}
        self.all = {"groupId": "pg-1", "directoryAdmin": "all", "orgCode": "", "orgCodes": None}

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
                "roleId": "role-1", "monitorCall": "all", "pttListen": "all"}

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


class MembersImportTests(unittest.TestCase):
    """POST members/import — CSV/JSON 행 → _member_write POST 와 같은 경로. 행 단위 결과, 한 행 실패가 다른 행을 막지 않는다."""

    def test_csv_header_normalisation_and_kinds(self):
        rows = dd._import_rows_from_csv(
            "\ufeffName,Org,Login_Id,Password,VoLTE_MSISDN,volte-password,ptt_msisdn,PTT_Service_Ref,ptt_sip_transport\n"
            "관제3석,TEAM01,disp03,pw3,+821310001003,sip3,+82510001003,mcptt,TLS\n"
            "\n"
            "현장D,TEAM02,,,,,+82510002004,,\n")
        self.assertEqual(rows, [
            {"name": "관제3석", "org": "TEAM01", "loginId": "disp03", "password": "pw3",
             "volte": {"msisdn": "+821310001003", "password": "sip3"},
             "ptt": {"msisdn": "+82510001003", "serviceRef": "mcptt", "sipTransport": "TLS"}},
            {"name": "현장D", "org": "TEAM02", "ptt": {"msisdn": "+82510002004"}},
        ])

    def test_import_calls_member_write_per_row_and_reports(self):
        calls = []

        async def fake_write(cur, config, scope, method, parts, body, actor, ip, my_uid):
            calls.append((method, parts, body))
            if body["name"] == "bad":
                return dd._json(403, {"error": "out_of_scope", "org": body.get("org")})
            return dd._json(201, {"userId": 100 + len(calls)})
        saved = dd._member_write
        dd._member_write = fake_write
        try:
            r = asyncio.run(dd._members_import(None, {}, {"all": True}, "name,org\nA,T1\nbad,T9\nB,T1\n", "+82131", "ip", 1))
            self.assertEqual(r.status, 200)
            self.assertEqual((r.body["created"], r.body["failed"]), (2, 1))
            self.assertEqual([x["status"] for x in r.body["results"]], [201, 403, 201])
            self.assertEqual(r.body["results"][1], {"row": 2, "status": 403, "error": "out_of_scope", "org": "T9"})
            self.assertEqual([m for m, _p, _b in calls], ["POST"] * 3)
            self.assertEqual(calls[0][1], ())
            # JSON rows — 같은 경로, name 없는 행은 호출 없이 400
            r = asyncio.run(dd._members_import(None, {}, {}, {"rows": [{"name": "C", "org": "T1"}, {"org": "T1"}]}, "a", "ip", 1))
            self.assertEqual([x["status"] for x in r.body["results"]], [201, 400])
            self.assertEqual(r.body["results"][1]["error"], "name_required")
            self.assertEqual(len(calls), 4)
            # 빈 CSV / 잘못된 본문 / 행 수 상한
            self.assertEqual(asyncio.run(dd._members_import(None, {}, {}, "name,org\n", "a", "ip", 1)).status, 400)
            self.assertEqual(asyncio.run(dd._members_import(None, {}, {}, 42, "a", "ip", 1)).status, 400)
            self.assertEqual(asyncio.run(dd._members_import(None, {}, {}, {"rows": [{"name": "x"}] * 501}, "a", "ip", 1)).status, 413)
        finally:
            dd._member_write = saved


class OamServiceTokenTests(unittest.TestCase):
    """CSC → OAM 프록시 자격 — 공유 CimsAuth.JwtSecret 로 서명한 단기 서비스 토큰(role=monitor). OAM 이력·녹취 API 의
    require_role 게이트를 그대로 통과해야 하므로 클레임 형태(sub/login_id/role)는 콘솔 admin JWT 와 같다."""

    def test_service_token_claims_and_headers(self):
        import jwt
        from services import admin_auth as aa
        saved = aa._SECRET
        try:
            aa.init({"CimsAuth": {"JwtSecret": "unit-secret"}})
            h = dr._oam_headers("application/json")
            self.assertEqual(h["Accept"], "application/json")
            self.assertTrue(h["Authorization"].startswith("Bearer "))
            claims = jwt.decode(h["Authorization"][7:], "unit-secret", algorithms=["HS256"])
            self.assertEqual((claims["sub"], claims["role"], claims["svc"]), ("csc", "monitor", "csc"))
            self.assertGreater(claims["exp"], claims["iat"])
            self.assertLessEqual(claims["exp"] - claims["iat"], 120)
            # OAM 쪽 검증 함수와 같은 규칙 — 다른 시크릿이면 무효
            with self.assertRaises(Exception):
                jwt.decode(h["Authorization"][7:], "other", algorithms=["HS256"])
            self.assertEqual(aa.role_rank(claims["role"]), aa.role_rank("monitor"))
        finally:
            aa._SECRET = saved


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
        rec_ptt = dh.query(t.sl, "ptt", dict(scope, roleId="role-1"), None, 10)[0][0]["recordingId"]
        rec_call = dh.query(t.sl, "call", dict(scope, roleId="role-1"), None, 10)[0][0]["recordingId"]
        self.assertTrue(dr.in_scope(t.sl, rec_ptt, scope, {"1": "g002"}))       # surrogate → mcptt id
        self.assertTrue(dr.in_scope(t.sl, rec_ptt, scope, {}))                   # session.json 대조 폴백
        self.assertFalse(dr.in_scope(t.sl, rec_ptt, {"members": set(), "ptt_groups": {"g009"}}, {"1": "g002"}))
        self.assertTrue(dr.in_scope(t.sl, rec_call, scope, {}))
        self.assertFalse(dr.in_scope(t.sl, rec_call, {"members": {"+8213107777"}, "ptt_groups": set()}, {}))
        self.assertFalse(dr.in_scope(t.sl, "message/x", scope, {}))

    def test_handler_gate_and_proxy(self):
        t = _Tree()
        t.ptt("g002", "ses-1", "+82510002001")
        rec = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}, "roleId": "role-1"}, None, 10)[0][0]["recordingId"]
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
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"roleId": "role-1", "groupId": "pg1", "members": set(), "ptt_groups": {"g002"}}, {"1": "g002"})
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
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"roleId": "role-1", "groupId": "pg1", "members": set(), "ptt_groups": {"g009"}}, {})
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
    """_admin_manages_group = authz.can(user, ptt_group.manage, group) — scope(관리 범위 안 org_code)·all·own(소유), 범위 밖·역할 없음은 거부."""

    def setUp(self):
        self._saved = (m._db_connect, dd.caller_identity, az.role_of, az.org_scope)
        m._db_connect = lambda: _Conn(types.SimpleNamespace())
        dd.caller_identity = lambda c, tok: ("+821310001001", 5020)
        az.org_scope = lambda cur, role, field='directory_write': ('own', 'TEAM01', {"TEAM01"})
        self.role = _role("own", 3, ptt_group_manage="scope")
        az.role_of = lambda cur, principal: self.role

    def tearDown(self):
        m._db_connect, dd.caller_identity, az.role_of, az.org_scope = self._saved

    def test_scope_decides(self):
        self.assertTrue(m._admin_manages_group({}, {"org_code": "TEAM01"}))
        self.assertFalse(m._admin_manages_group({}, {"org_code": "TEAM02"}))
        self.assertFalse(m._admin_manages_group({}, {"org_code": ""}))
        self.assertTrue(m._admin_manages_group({}, None))                     # 신규 생성 = scope|all
        self.assertTrue(m._admin_manages_group({}, {"org_code": "TEAM02", "authorized_user_id": 5020}))   # 내 소유
        self.role = _role("own", 3)                                            # ptt_group_manage none 이어도 관리 범위가 scope 를 유도(§3.4)
        self.assertTrue(m._admin_manages_group({}, {"org_code": "TEAM01"}))
        self.role = _role("none", None, ptt_group_manage="own")
        self.assertFalse(m._admin_manages_group({}, None))                    # own 은 생성 인가가 아니다
        self.assertTrue(m._admin_manages_group({}, {"org_code": "X", "authorized_user_id": 5020}))
        self.role = _role("none", None, ptt_group_manage="all")
        self.assertTrue(m._admin_manages_group({}, {"org_code": "TEAM02"}))
        self.role = None
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
        self.assertEqual(out["volte"], [{"name": "volte", "domain": "ims.example", "kind": "volte"}])
        self.assertEqual(out["ptt"], [{"name": "mcptt", "domain": "ptt.example", "kind": "ptt"}])

    def test_fallback_without_name_uses_kind(self):
        cfg = {"Provisioning": {"Services": {"ptt": {"domain": "ptt.example"}}}}
        self.assertEqual(dd._services(cfg)["ptt"], [{"name": "ptt", "domain": "ptt.example", "kind": "ptt"}])

    def test_voip_rides_volte_bucket_with_kind(self):
        """유선 voip 접속환경은 전화 회선 버킷(volte_subscriptions)에 실리고 항목 kind 로 구분된다(sip_service_model.md §2-9)."""
        cfg = {"Provisioning": {"Services": {
            "volte": {"name": "volte", "domain": "volte.example"},
            "voip": {"name": "voip", "domain": "voip.example"},
            "ptt": {"name": "mcptt", "domain": "ptt.example"}}}}
        out = dd._services(cfg)
        self.assertEqual(out["volte"], [{"name": "volte", "domain": "volte.example", "kind": "volte"},
                                        {"name": "voip", "domain": "voip.example", "kind": "voip"}])
        self.assertEqual([x["kind"] for x in out["ptt"]], ["ptt"])

    def test_runtime_store_rows_carry_kind(self):
        """runtime store access_services 가 있으면 그것이 우선 — mcptt 종류는 ptt 버킷·kind ptt, voip 는 volte 버킷·kind voip."""
        import services.file_store as fs
        fs.load_all = lambda d: [
            {"name": "voip", "kind": "voip", "domain": "voip.example", "priority": 150},
            {"name": "volte", "kind": "volte", "domain": "volte.example", "priority": 100},
            {"name": "mcptt", "kind": "mcptt", "domain": "ptt.example", "priority": 100},
            {"kind": "volte", "domain": "no-name.example", "priority": 1},   # name 없음 = 후보 아님
        ]   # 후보 순서 = priority 오름차순(같으면 name) — services/access_services.records
        out = dd._services({"Provisioning": {"Services": {}}})
        self.assertEqual([(x["name"], x["kind"]) for x in out["volte"]], [("volte", "volte"), ("voip", "voip")])
        self.assertEqual(out["ptt"], [{"name": "mcptt", "domain": "ptt.example", "kind": "ptt"}])


class _GroupsCur:
    """_groups_in_scope 가 내는 SQL 만 흉내 — ptt_groups 5개(g1 TEAM01 소속·g2 조직 없음·g3 조직 없음·g4 TEAM02·g5 내 소유)."""

    GROUPS = [
        {"id": 1, "mcptt_group_id": "g001", "name": "a", "org_code": "TEAM01", "authorized_user_id": 9, "group_type": "prearranged"},
        {"id": 2, "mcptt_group_id": "g002", "name": "b", "org_code": None, "authorized_user_id": 9, "group_type": "prearranged"},
        {"id": 3, "mcptt_group_id": "g003", "name": "c", "org_code": None, "authorized_user_id": None, "group_type": "prearranged"},
        {"id": 4, "mcptt_group_id": "g004", "name": "d", "org_code": "TEAM02", "authorized_user_id": None, "group_type": "chat"},
        {"id": 5, "mcptt_group_id": "g005", "name": "e", "org_code": None, "authorized_user_id": 5020, "group_type": "prearranged"},
    ]

    def __init__(self, ptt_listen, targets, member_groups):
        self.ptt_listen, self.targets, self.member_groups = ptt_listen, targets, member_groups
        self._rows = []

    def execute(self, q, args=None):
        if q.startswith("SELECT id, mcptt_group_id, name, org_code"):
            self._rows = list(self.GROUPS)
        elif q.startswith("SELECT group_id, COUNT(*)"):
            self._rows = [{"group_id": 1, "n": 3}, {"group_id": 2, "n": 4}]
        elif q.startswith("SHOW TABLES LIKE 'roles'"):
            self._rows = [{"x": 1}]
        elif q.startswith("SELECT phone_group_id FROM role_monitor_targets"):
            self._rows = []
        elif q.startswith("SELECT ptt_group_id FROM role_ptt_targets"):
            self._rows = [{"ptt_group_id": i} for i in self.targets]
        elif q.startswith("SELECT gm.group_id FROM ptt_group_members"):
            self._rows = [{"group_id": i} for i in self.member_groups]
        else:
            raise AssertionError("unexpected SQL: " + q)

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)


class GroupsInScopeTests(unittest.TestCase):
    """PTT 그룹 관리 열거 = 관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 멤버 그룹, canManage 는 역할 ptt_group_manage(scope=관리 범위)·소유만(GMS 게이트와 동일)."""

    def setUp(self):
        az.reset_probe()

    @staticmethod
    def _own(ptt_listen):
        return {"roleId": "role-1", "groupId": "pg-1", "directoryWrite": "own", "directoryAdmin": "own", "orgCode": "TEAM01",
                "orgCodes": {"TEAM01"}, "role": _role("own", 3, ptt_listen=ptt_listen, ptt_group_manage="scope")}

    def _ids(self, rows, key=None):
        return [r["id"] for r in rows if key is None or r[key]]

    def test_own_scope_shows_listen_and_member_groups_read_only(self):
        rows = dd._groups_in_scope(_GroupsCur("listed", targets={2, 3}, member_groups={3}), self._own("listed"), 5020)
        self.assertEqual(self._ids(rows), ["g001", "g002", "g003", "g005"])          # g004(TEAM02) 만 밖
        self.assertEqual(self._ids(rows, "canManage"), ["g001", "g005"])            # 범위 안 조직 · 내 소유
        self.assertEqual(self._ids(rows, "inListenScope"), ["g002", "g003"])
        self.assertEqual(self._ids(rows, "isMember"), ["g003"])
        self.assertEqual(rows[0]["memberCount"], 3)

    def test_listen_all_shows_everything(self):
        rows = dd._groups_in_scope(_GroupsCur("all", targets=set(), member_groups=set()), self._own("all"), 5020)
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(r["inListenScope"] for r in rows))
        self.assertEqual(self._ids(rows, "canManage"), ["g001", "g005"])

    def test_no_listen_no_member_is_manage_only(self):
        rows = dd._groups_in_scope(_GroupsCur("none", targets=set(), member_groups=set()), self._own("none"), None)
        self.assertEqual(self._ids(rows), ["g001"])

    def test_all_scope_manages_everything(self):
        sc = {"roleId": "role-1", "groupId": "", "directoryWrite": "all", "directoryAdmin": "all", "orgCode": "", "orgCodes": None,
              "role": _role("all", None, ptt_group_manage="scope")}
        rows = dd._groups_in_scope(_GroupsCur("none", targets=set(), member_groups=set()), sc, 5020)
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(r["canManage"] for r in rows))

    def test_ptt_group_manage_all_without_directory_scope(self):
        sc = {"roleId": "role-1", "groupId": "", "directoryWrite": "none", "directoryAdmin": "none", "orgCode": "", "orgCodes": set(),
              "role": _role("none", None, ptt_group_manage="all", ptt_listen="none")}
        rows = dd._groups_in_scope(_GroupsCur("none", targets=set(), member_groups=set()), sc, 5020)
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(r["canManage"] for r in rows))


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
        self.scope = {"groupId": "pg-1", "directoryAdmin": "own", "orgCode": "TEAM01", "orgCodes": {"TEAM01"}}

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


class PttSessionDetailTests(unittest.TestCase):
    """GET /provisioning/history/ptt/{recordingId} — 녹취와 같은 범위 게이트 + OAM 세션 이벤트/floor 합본 프록시."""

    def test_session_ref(self):
        self.assertEqual(dr._ptt_session_ref("ptt/3/2026/09/06/19/S20260906190102000000_1"), ("3", "S20260906190102000000_1"))
        self.assertEqual(dr._ptt_session_ref("ptt/3/2026/09/06/19"), ("3", "2026090619"))          # 구 녹취 = 시간창
        self.assertEqual(dr._ptt_session_ref("volte/2026/09/06/19/010/0100/c1.d"), (None, None))
        self.assertEqual(dr._ptt_session_ref("ptt/3"), (None, None))

    def test_fetch_ptt_sessions_params_and_failures(self):
        calls = []

        class _Resp:
            def __init__(self, status, content):
                self.status_code, self.headers, self.content = status, {"content-type": "application/json"}, content

        class _Http:
            status = 200
            def get(self, url, params=None, headers=None, timeout=None):
                calls.append((url, params))
                return _Resp(_Http.status, json.dumps({"items": [{"dir": "S1_1"}]}).encode())
        saved = dr._http
        dr._http = lambda cfg: _Http()
        try:
            from datetime import datetime as _dt
            cfg = {"Fm": {"OamIp": "10.0.0.1"}}
            self.assertEqual(dr.fetch_ptt_sessions(cfg, _dt(2026, 9, 6), _dt(2026, 9, 6, 23), set()), [])   # 범위 그룹 없음 → OAM 호출 안 함
            self.assertEqual(calls, [])
            items = dr.fetch_ptt_sessions(cfg, _dt(2026, 9, 6, 0, 0), _dt(2026, 9, 6, 23, 59), {"3", "1"})
            self.assertEqual(items, [{"dir": "S1_1"}])
            self.assertEqual(calls[-1][0], "https://10.0.0.1:4419/api/v1/ptt/sessions")
            self.assertEqual(calls[-1][1], {"group_key": "1,3", "limit": "1000", "date": "2026-09-06"})
            dr.fetch_ptt_sessions(cfg, _dt(2026, 9, 5, 23, 0), _dt(2026, 9, 6, 1, 0), {"3"})
            self.assertEqual((calls[-1][1]["from"], calls[-1][1]["to"]), ("2026-09-05", "2026-09-06"))
            _Http.status = 500
            self.assertIsNone(dr.fetch_ptt_sessions(cfg, _dt(2026, 9, 6), _dt(2026, 9, 6, 23), {"3"}))       # 비정상 → 폴백 신호
        finally:
            dr._http = saved

    def test_detail_gate_and_proxy(self):
        t = _Tree()
        # 세션키형 디렉터리(콘솔/OAM 세션 인덱스 키) — 구 녹취형은 _Tree.ptt 가 만든다
        y, mo, d, h = _now_parts(t.now)
        ses = "S20260906190102000000_1"
        os.makedirs(os.path.join(t.sl, "ptt", "1", y, mo, d, h, ses), exist_ok=True)
        with open(os.path.join(t.sl, "ptt", "1", y, mo, d, h, ses, "session.json"), "w") as f:
            json.dump({"mcptt_group_id": "g002", "sesid": "ses-9", "start_time": t.ts(5)}, f)
        rec = f"ptt/1/{y}/{mo}/{d}/{h}/{ses}"
        import services as _svc_pkg
        saved = (m.extract_token, m._SERVICE_LOG_DIR, dr._scope_sets, dr._http,
                 sys.modules.get("services.fm_reporter"), getattr(_svc_pkg, "fm_reporter", None))
        audits = []
        fake_fm = types.SimpleNamespace(get=lambda: types.SimpleNamespace(node="n1", send_event=lambda *a, **k: audits.append(k)))
        sys.modules["services.fm_reporter"] = fake_fm
        _svc_pkg.fm_reporter = fake_fm
        try:
            token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
            m.extract_token = lambda hdr: token if hdr else None
            m._SERVICE_LOG_DIR = t.sl
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"roleId": "role-1", "groupId": "pg1", "members": set(), "ptt_groups": {"g002"}}, {"1": "g002"})
            calls = []

            class _Resp:
                def __init__(self, status, content):
                    self.status_code, self.headers, self.content = status, {"content-type": "application/json"}, content

            class _Http:
                def get(self, url, params=None, headers=None, timeout=None):
                    calls.append(url)
                    if url.endswith("/floor"):
                        return _Resp(200, json.dumps({"floor": [{"ts": "2026-09-06T19:01:03", "op": "GRANT", "user": "+8250001"}]}).encode())
                    return _Resp(200, json.dumps({"session": {"sesid": "ses-9"}, "events": [{"ts": "2026-09-06T19:01:02", "type": "session_start"}],
                                                  "participants": [{"msisdn": "+8250001", "role": "initiator", "join_time": None, "leave_time": None}],
                                                  "has_recording": True}).encode())
            dr._http = lambda cfg: _Http()

            def call(path, token_hdr="Bearer x"):
                a = HandlerArgs("GET", path, "127.0.0.1", 0, headers={"authorization": token_hdr} if token_hdr else {}, query_params={})
                return asyncio.run(dr.handle_ptt_session_detail(a, {"config": {"Fm": {"OamIp": "10.0.0.1"}}}))

            self.assertEqual(call("/provisioning/history/ptt/" + rec, token_hdr="").status, 401)
            self.assertEqual(call("/provisioning/history/ptt/../x").status, 400)
            self.assertEqual(call("/provisioning/history/ptt/volte/2026/09/06/19/a/b/c.d").status, 400)
            self.assertEqual(call("/provisioning/history/ptt/ptt/1/2026/01/01/00/S20260101000000000000_1").status, 404)
            r = call("/provisioning/history/ptt/" + rec)
            self.assertEqual(r.status, 200)
            self.assertEqual(r.body["recordingId"], rec)
            self.assertEqual(r.body["session"]["sesid"], "ses-9")
            self.assertEqual(r.body["participants"][0]["role"], "initiator")
            self.assertEqual(r.body["events"][0]["type"], "session_start")
            self.assertEqual(r.body["floor"][0]["op"], "GRANT")
            self.assertTrue(r.body["hasRecording"])
            self.assertEqual(calls, [f"https://10.0.0.1:4419/api/v1/ptt/history/1/{ses}",
                                     f"https://10.0.0.1:4419/api/v1/ptt/history/1/{ses}/floor"])
            self.assertEqual([a["params"]["tap_mode"] for a in audits], ["history"])
            self.assertEqual(audits[0]["params"]["hist_kind"], "ptt_session")
            # 범위 밖 / 관제 미소속
            dr._scope_sets = lambda cfg, tok: ("+821310001001", {"roleId": "role-1", "groupId": "pg1", "members": set(), "ptt_groups": {"g009"}}, {})
            self.assertEqual(call("/provisioning/history/ptt/" + rec).body["error"], "out_of_scope")
            dr._scope_sets = lambda cfg, tok: ("+821310001001", None, {})
            self.assertEqual(call("/provisioning/history/ptt/" + rec).body["error"], "no_monitor_scope")
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
