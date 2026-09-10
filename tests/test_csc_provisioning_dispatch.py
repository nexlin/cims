"""csc/src/services/mcptt.py — /provisioning/me 전화 그룹·관제 역할 발견(discovery) 단위 시험 (오프라인, DB 없음).

dispatch_center.md §8.4 / android_ue_provisioning.md §3: `phoneGroup`(소속 전화 그룹 + 같은 그룹원) 과 `dispatch`(배정 역할 +
서버가 CSP CanWatch/CanListenPtt 와 같은 규칙으로 monitor_call/ptt_listen 을 해석한 members[]/pttTargets[]) 두 블록, 전환기
합성 필드(groupId/groupName/pilotId/monitorScope/directoryAdmin), 블록 etag, 응답 ETag + If-None-Match 304.
가짜 커서가 dispatch_discovery·services/authz 가 내는 SQL 만 흉내 낸다.

  python3 -m unittest tests.test_csc_provisioning_dispatch
"""
from __future__ import annotations

import asyncio
import os
import sys
import types
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

import services.mcptt as m  # noqa: E402
import services.authz as az  # noqa: E402
import handlers.dispatch as hd  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

PG = "pg-dispatch01"
# users: id → (name)
USERS = {5020: "관제1석", 5021: "관제2석", 5030: "현장A", 5031: "현장B", 5040: "타부서", 5050: "현장C(PTT전용)"}
# volte_subscriptions: msisdn → user_id
VOLTE = {"+821310001001": 5020, "+821310001002": 5021, "+821310002001": 5030, "+821310002002": 5031,
         "+821310003001": 5040}
# subscriptions.service_ref(접속서비스 name) — 기본 빈값(종류 키 폴백). voip 시험이 한 회선만 바꾼다.
SREF: dict[str, str] = {}
# ptt_subscriptions: msisdn → user_id (관제2석은 PTT 미가입, 현장A 는 PTT 2회선)
PTT = {"+82510001001": 5020, "+82510002001": 5030, "+82510002009": 5030, "+82510002002": 5031,
       "+82510005001": 5050}   # 5050 = PTT 전용(VoLTE 회선 없음) — monitor_call=all 감시 대상(dispatch_center.md §5.6a)
# phone_group_members: volte msisdn → (group_id, alert_order)
PGM = {"+821310001002": (PG, 2), "+821310001001": (PG, 1),
       "+821310002001": ("pg-field", 1), "+821310002002": ("pg-field", 2)}
GROUPS = {PG: ("관제 1조", "+821310001000"), "pg-field": ("현장", "")}
ORGS = {3: "TEAM01"}
PTT_GROUPS = [(24, "g002", "음성그룹2"), (25, "g001", "음성그룹1"), (30, "g-0a1b2c3d", "관제임시")]
ROLE_ID = "role-dispatch01"
MONITOR_TARGETS = {ROLE_ID: {"pg-field"}}
PTT_TARGETS = {ROLE_ID: {24, 30}}


def _role(scope="own", ptt_listen="none", vis="hidden", dw="none", org_id=None, name="관제 1조 감독"):
    """roles 행(튜플 커서 — authz.ROLE_COLS 순서)."""
    return {'id': ROLE_ID, 'name': name, 'builtin': 0, 'authz_manage': 0, 'audit_read': 0, 'directory_write': dw,
            'directory_read': 'none', 'ptt_group_manage': 'none', 'monitor_call': scope, 'ptt_listen': ptt_listen,
            'listen_visibility': vis, 'history_read': 'scope', 'alarm_ack': 0, 'mcptt_control': 0, 'org_id': org_id}


class _FakeCursor:
    """dispatch_discovery / services.authz / handle_provisioning_me 가 내는 SQL 만 흉내 내는 tuple 커서."""

    def __init__(self, role: dict | None, assigned=(5020, 5021), tables: bool = True):
        self.role = role                  # roles 행 (None = 역할 없음)
        self.assigned = set(assigned)     # 배정된 person
        self.tables = tables
        self.sql: list[tuple[str, tuple]] = []
        self._rows: list = []

    # ── 도우미 ──
    @staticmethod
    def _group_of_person(uid):
        cands = sorted((o, vid, gid) for vid, (gid, o) in PGM.items() if VOLTE.get(vid) == uid)
        return cands[0][2] if cands else None

    def _member_rows(self, group_ids, own):
        rows = []
        for vid, uid in VOLTE.items():
            mg, order = PGM.get(vid, ("", 0))
            if group_ids is not None and mg not in group_ids:
                continue
            ptt_ids = sorted(p for p, u in PTT.items() if u == uid)
            rows.append((uid, USERS[uid], vid, mg, ptt_ids[0] if ptt_ids else None, order))
        # ORDER BY CASE WHEN m.group_id=own THEN 0 ELSE 1 END, m.group_id, m.alert_order, s.id
        rows.sort(key=lambda r: (0 if r[3] == own else 1, r[3], r[5], r[2]))
        return [r[:5] for r in rows]

    # ── DB-API ──
    def execute(self, q, args=None):
        self.sql.append((q, tuple(args) if args is not None else ()))
        self._rows = []
        if not self.tables and ("phone_group" in q or "role" in q):
            if q.startswith("SHOW TABLES"):
                return
            raise RuntimeError("(1146, \"Table 'cims.phone_group_members' doesn't exist\")")
        if q.startswith("SHOW TABLES LIKE"):
            self._rows = [("x",)]
        elif q.startswith("SELECT user_id FROM volte_subscriptions WHERE id="):
            uid = VOLTE.get(args[0])
            self._rows = [(uid,)] if uid is not None else []
        elif q.startswith("SELECT user_id FROM ptt_subscriptions WHERE id="):
            uid = PTT.get(args[0])
            self._rows = [(uid,)] if uid is not None else []
        elif q.startswith("SELECT id, imsi, auth_id, sip_transport, ha1, auth_scheme") and "volte_subscriptions" in q:
            # 10열 — 마지막 COALESCE(service_ref,'') 는 접속서비스 name(sip_service_model.md §2-9). 빈값 = 종류 키 폴백.
            self._rows = [(vid, "45033" + vid[-10:], "", "TLS", "0" * 32, "digest", "", "", "", SREF.get(vid, ""))
                          for vid, uid in sorted(VOLTE.items()) if uid == args[0]]
        elif q.startswith("SELECT id, imsi, auth_id, sip_transport, ha1, auth_scheme") and "ptt_subscriptions" in q:
            self._rows = [(pid, "45033" + pid[-10:], "", "TLS", "0" * 32, "digest", "", "", "", SREF.get(pid, ""))
                          for pid, uid in sorted(PTT.items()) if uid == args[0]]
        elif q.startswith("SELECT name FROM users WHERE id="):
            self._rows = [(USERS[args[0]],)] if args[0] in USERS else []
        elif q.startswith("SELECT m.group_id FROM phone_group_members m WHERE m.user_id IN ("):
            g = self._group_of_person(args[0])
            self._rows = [(g,)] if g else []
        elif q.startswith("SELECT g.id, g.name, COALESCE(g.pilot_id,'') FROM phone_groups g WHERE g.id="):
            g = GROUPS.get(args[0])
            self._rows = [(args[0], g[0], g[1])] if g else []
        elif q.startswith("SELECT role_id FROM role_assignments WHERE principal_type='user' AND principal_id="):
            self._rows = [(ROLE_ID,)] if self.role and int(args[0]) in self.assigned else []
        elif q.startswith("SELECT " + ", ".join(az.ROLE_COLS) + " FROM roles WHERE id="):
            self._rows = [tuple(self.role[c] for c in az.ROLE_COLS)] if self.role and args[0] == ROLE_ID else []
        elif q.startswith("SELECT phone_group_id FROM role_monitor_targets WHERE role_id="):
            self._rows = [(g,) for g in sorted(MONITOR_TARGETS.get(args[0], set()))]
        elif q.startswith("SELECT ptt_group_id FROM role_ptt_targets WHERE role_id="):
            self._rows = [(g,) for g in sorted(PTT_TARGETS.get(args[0], set()))]
        elif q.startswith("SELECT code FROM organizations WHERE id="):
            self._rows = [(ORGS[args[0]],)] if args[0] in ORGS else []
        elif q.startswith(m._MEMBER_SQL):
            own = args[-1]
            if " WHERE m.group_id IN (" in q:
                self._rows = self._member_rows(set(args[:-1]), own)
            else:
                self._rows = self._member_rows(None, own)
        elif q.startswith("SELECT u.id, u.name, MIN(p.id) FROM ptt_subscriptions p JOIN users u"):
            volte_uids = set(VOLTE.values())
            rows = {}
            for pid, uid in sorted(PTT.items()):
                if uid in volte_uids or uid in rows:
                    continue
                rows[uid] = (uid, USERS[uid], pid)
            self._rows = sorted(rows.values(), key=lambda r: r[2])
        elif q.startswith("SELECT mcptt_group_id, name FROM ptt_groups"):
            self._rows = sorted(((g, n) for _pk, g, n in PTT_GROUPS))
        elif q.startswith("SELECT g.mcptt_group_id, g.name FROM role_ptt_targets"):
            pks = PTT_TARGETS.get(args[0], set())
            self._rows = sorted(((g, n) for pk, g, n in PTT_GROUPS if pk in pks))
        else:
            raise AssertionError(f"unexpected SQL: {q}")

    def fetchall(self):
        return list(self._rows)

    def fetchone(self):
        return self._rows[0] if self._rows else None


class _FakeConn:
    def __init__(self, cur):
        self.cur = cur

    def cursor(self):
        return self.cur

    def close(self):
        pass


class _Base(unittest.TestCase):
    def setUp(self):
        self._prov = m.PROVISIONING
        m.PROVISIONING = {}
        hd._HAS_TABLES = None
        az.reset_probe()

    def tearDown(self):
        m.PROVISIONING = self._prov
        hd._HAS_TABLES = None
        az.reset_probe()

    def _disc(self, cur, uid):
        az.reset_probe()                  # 시험마다 역할 행이 다르다 — 캐시 무효
        return m.dispatch_discovery(cur, uid)


class DiscoveryTests(_Base):
    def test_no_group_no_role_is_nothing(self):
        d = self._disc(_FakeCursor(_role()), 5040)
        self.assertEqual(d, {"phoneGroup": None, "dispatch": None})

    def test_phone_group_block_members_are_group_in_alert_order(self):
        d = self._disc(_FakeCursor(None), 5021)
        pg = d["phoneGroup"]
        self.assertEqual((pg["groupId"], pg["groupName"], pg["pilotId"]), (PG, "관제 1조", "+821310001000"))
        self.assertEqual([x["volteAor"] for x in pg["members"]], ["tel:+821310001001", "tel:+821310001002"])
        self.assertEqual(set(pg["members"][0]), {"userId", "name", "volteAor", "pttId", "extension"})   # groupId 없음
        me = pg["members"][1]
        self.assertEqual((me["userId"], me["name"], me["extension"]), (5021, "관제2석", "1002"))
        self.assertRegex(pg["etag"], r'^"[0-9a-f]{32}"$')

    def test_phone_group_only_synthesises_dispatch_with_none_scopes(self):
        """전화 그룹만 있고 역할이 없으면 dispatch 는 합성 필드 + members[](그룹원) 만, 범위는 none (android_ue_provisioning §3)."""
        d = self._disc(_FakeCursor(None), 5020)
        dp = d["dispatch"]
        self.assertNotIn("roleId", dp)
        self.assertEqual((dp["groupId"], dp["groupName"], dp["pilotId"]), (PG, "관제 1조", "+821310001000"))
        self.assertEqual((dp["monitorScope"], dp["pttListen"], dp["listenVisibility"], dp["directoryAdmin"], dp["orgCode"]),
                         ("none", "none", "hidden", "none", ""))
        self.assertEqual([x["groupId"] for x in dp["members"]], [PG, PG])
        self.assertEqual(dp["pttTargets"], [])

    def test_role_own_members_are_own_group(self):
        d = self._disc(_FakeCursor(_role("own")), 5021)
        dp = d["dispatch"]
        self.assertEqual((dp["roleId"], dp["roleName"], dp["monitorCall"], dp["monitorScope"]), (ROLE_ID, "관제 1조 감독", "own", "own"))
        self.assertEqual([x["volteAor"] for x in dp["members"]], ["tel:+821310001001", "tel:+821310001002"])
        self.assertTrue(all(x["groupId"] == PG for x in dp["members"]))
        self.assertEqual(dp["pttTargets"], [])
        self.assertEqual((dp["groupId"], dp["pilotId"]), (PG, "+821310001000"))                  # 합성 필드
        self.assertEqual((dp["directoryWrite"], dp["directoryAdmin"], dp["orgCode"]), ("none", "none", ""))

    def test_role_none_still_lists_own_group(self):
        """CanWatch 규칙 1 — 같은 픽업 그룹은 monitor_call 과 무관하게 허용."""
        dp = self._disc(_FakeCursor(_role("none")), 5020)["dispatch"]
        self.assertEqual(len(dp["members"]), 2)
        self.assertEqual(dp["monitorCall"], "none")

    def test_role_without_phone_group(self):
        """역할만 있고 전화 그룹이 없는 사람 — own 은 빈 목록, all 은 전 가입자. 합성 groupId 는 ''."""
        cur = _FakeCursor(_role("own"), assigned=(5040,))
        d = self._disc(cur, 5040)
        self.assertIsNone(d["phoneGroup"])
        self.assertEqual((d["dispatch"]["groupId"], d["dispatch"]["members"]), ("", []))
        d = self._disc(_FakeCursor(_role("all"), assigned=(5040,)), 5040)
        self.assertEqual(len(d["dispatch"]["members"]), len(VOLTE) + 1)

    def test_ptt_id_first_subscription_or_empty(self):
        dp = self._disc(_FakeCursor(_role("listed")), 5020)["dispatch"]
        by_uid = {x["userId"]: x for x in dp["members"]}
        self.assertEqual(by_uid[5020]["pttId"], "tel:+82510001001")
        self.assertEqual(by_uid[5021]["pttId"], "")                    # PTT 미가입
        self.assertEqual(by_uid[5030]["pttId"], "tel:+82510002001")   # 2회선 → MIN(id)

    def test_listed_adds_target_group_members_after_own(self):
        dp = self._disc(_FakeCursor(_role("listed")), 5020)["dispatch"]
        self.assertEqual([x["groupId"] for x in dp["members"]], [PG, PG, "pg-field", "pg-field"])
        self.assertNotIn(5040, [x["userId"] for x in dp["members"]])   # 무소속 가입자는 listed 범위 밖

    def test_all_is_every_volte_subscriber_like_csp_canwatch(self):
        dp = self._disc(_FakeCursor(_role("all")), 5020)["dispatch"]
        self.assertEqual(len(dp["members"]), len(VOLTE) + 1)            # + PTT 전용 가입자 1명(뒤에 붙는다)
        self.assertEqual([x["groupId"] for x in dp["members"][:2]], [PG, PG])   # 자기 그룹 먼저
        by_uid = {x["userId"]: x for x in dp["members"]}
        self.assertEqual(by_uid[5040]["groupId"], "")                  # 전화 그룹 없는 가입자 groupId=""
        self.assertEqual(by_uid[5030]["groupId"], "pg-field")
        self.assertEqual(by_uid[5050], {"userId": 5050, "name": "현장C(PTT전용)", "volteAor": "",
                                        "pttId": "tel:+82510005001", "extension": "5001", "groupId": ""})
        self.assertEqual(dp["members"][-1]["userId"], 5050)

    def test_own_and_listed_do_not_add_ptt_only_users(self):
        for scope in ("own", "listed", "none"):
            cur = _FakeCursor(_role(scope))
            dp = self._disc(cur, 5020)["dispatch"]
            self.assertNotIn(5050, [x["userId"] for x in dp["members"]], scope)
            self.assertFalse(any(q.startswith("SELECT u.id, u.name, MIN(p.id) FROM ptt_subscriptions") for q, _ in cur.sql), scope)

    def test_ptt_targets_listed_and_all_use_tel_uri(self):
        dp = self._disc(_FakeCursor(_role(ptt_listen="listed")), 5020)["dispatch"]
        self.assertEqual(dp["pttTargets"], [{"id": "g-0a1b2c3d", "uri": "tel:g-0a1b2c3d", "name": "관제임시"},
                                            {"id": "g002", "uri": "tel:g002", "name": "음성그룹2"}])
        dp = self._disc(_FakeCursor(_role(ptt_listen="all")), 5020)["dispatch"]
        self.assertEqual([t["id"] for t in dp["pttTargets"]], ["g-0a1b2c3d", "g001", "g002"])
        self.assertEqual(dp["pttListen"], "all")

    def test_directory_write_and_org_code(self):
        dp = self._disc(_FakeCursor(_role(dw="own", org_id=3)), 5020)["dispatch"]
        self.assertEqual((dp["directoryWrite"], dp["directoryAdmin"], dp["orgCode"]), ("own", "own", "TEAM01"))
        dp = self._disc(_FakeCursor(_role(dw="all")), 5020)["dispatch"]
        self.assertEqual((dp["directoryWrite"], dp["orgCode"]), ("all", ""))

    def test_extension_digits_setting(self):
        m.PROVISIONING = {"ExtensionDigits": 3}
        d = self._disc(_FakeCursor(_role()), 5020)
        self.assertEqual(d["phoneGroup"]["members"][0]["extension"], "001")
        m.PROVISIONING = {"ExtensionDigits": 0}
        d = self._disc(_FakeCursor(_role()), 5020)
        self.assertEqual(d["dispatch"]["members"][0]["extension"], "821310001001")
        m.PROVISIONING = {"ExtensionDigits": "bogus"}
        d = self._disc(_FakeCursor(_role()), 5020)
        self.assertEqual(d["dispatch"]["members"][0]["extension"], "1001")

    def test_block_etags_are_content_derived(self):
        a = self._disc(_FakeCursor(_role("own")), 5020)
        b = self._disc(_FakeCursor(_role("own")), 5020)
        c = self._disc(_FakeCursor(_role("all")), 5020)
        for key in ("phoneGroup", "dispatch"):
            self.assertEqual(a[key]["etag"], b[key]["etag"])
            self.assertRegex(a[key]["etag"], r'^"[0-9a-f]{32}"$')
            probe = {k: v for k, v in a[key].items() if k != "etag"}
            self.assertEqual(m._content_etag_json(probe), a[key]["etag"])
        self.assertNotEqual(a["dispatch"]["etag"], c["dispatch"]["etag"])
        self.assertEqual(a["phoneGroup"]["etag"], c["phoneGroup"]["etag"])       # 전화 그룹은 역할과 무관

    def test_sql_shapes(self):
        """CSP 와 같은 규칙임을 SQL 로 고정 — listed 는 자기 그룹 ∪ role_monitor_targets, all 은 WHERE 없음, PTT 는 role_ptt_targets."""
        cur = _FakeCursor(_role("listed", "listed"))
        self._disc(cur, 5020)
        member_qs = [(q, a) for q, a in cur.sql if q.startswith(m._MEMBER_SQL)]
        q, a = member_qs[-1]
        self.assertIn(" WHERE m.group_id IN (%s,%s)", q)
        self.assertEqual(set(a[:-1]), {PG, "pg-field"})
        self.assertIn("ORDER BY CASE WHEN m.group_id=%s THEN 0 ELSE 1 END", q)
        self.assertTrue(any(q.startswith("SELECT phone_group_id FROM role_monitor_targets") for q, _ in cur.sql))
        self.assertTrue(any(q.startswith("SELECT g.mcptt_group_id, g.name FROM role_ptt_targets") for q, _ in cur.sql))
        cur = _FakeCursor(_role("all", "all"))
        self._disc(cur, 5020)
        q, a = [(q, a) for q, a in cur.sql if q.startswith(m._MEMBER_SQL)][-1]
        self.assertNotIn(" WHERE m.group_id", q)
        self.assertTrue(any(q == "SELECT mcptt_group_id, name FROM ptt_groups ORDER BY mcptt_group_id" for q, _ in cur.sql))


class HandlerTests(_Base):
    """handle_provisioning_me — 두 블록 탑재·ETag·If-None-Match 304·테이블 미적용/미소속 생략."""

    def setUp(self):
        super().setUp()
        self._saved = (m._DB_CONFIG, m.extract_token, sys.modules.get("pymysql"))
        m._DB_CONFIG = {"Host": "127.0.0.1", "Port": 3306, "User": "cims", "Password": "", "Db": "cims"}
        self.token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
        m.extract_token = lambda hdr: self.token if hdr else None
        self.cur = _FakeCursor(_role("own", "listed", dw="own", org_id=3))
        sys.modules["pymysql"] = types.SimpleNamespace(connect=lambda **kw: _FakeConn(self.cur))

    def tearDown(self):
        m._DB_CONFIG, m.extract_token, pm = self._saved
        if pm is None:
            sys.modules.pop("pymysql", None)
        else:
            sys.modules["pymysql"] = pm
        super().tearDown()

    def _get(self, headers=None):
        az.reset_probe()
        h = {"authorization": "Bearer x", "host": "csc.test:4430"}
        h.update(headers or {})
        return asyncio.run(m.handle_provisioning_me(HandlerArgs("GET", "/provisioning/me", "127.0.0.1", 0, headers=h), {}))

    def test_two_blocks_and_etag_header(self):
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertRegex(r.headers.get("ETag", ""), r'^"[0-9a-f]{32}"$')
        pg, d = r.body["phoneGroup"], r.body["dispatch"]
        self.assertEqual(pg["groupId"], PG)
        self.assertEqual([x["extension"] for x in pg["members"]], ["1001", "1002"])
        self.assertEqual((d["roleId"], d["monitorCall"], d["pttListen"], d["directoryWrite"], d["orgCode"]),
                         (ROLE_ID, "own", "listed", "own", "TEAM01"))
        self.assertEqual([t["uri"] for t in d["pttTargets"]], ["tel:g-0a1b2c3d", "tel:g002"])
        self.assertEqual((d["groupId"], d["monitorScope"], d["directoryAdmin"]), (PG, "own", "own"))     # 전환기 합성
        self.assertEqual(d["etag"], m._content_etag_json({k: v for k, v in d.items() if k != "etag"}))
        self.assertEqual([s["kind"] for s in r.body["services"]], ["volte", "ptt"])

    def test_if_none_match_304(self):
        first = self._get()
        again = self._get({"if-none-match": first.headers["ETag"]})
        self.assertEqual(again.status, 304)
        self.assertEqual(again.headers["ETag"], first.headers["ETag"])
        self.assertIsNone(again.body)
        stale = self._get({"If-None-Match": '"deadbeef"'})
        self.assertEqual(stale.status, 200)

    def test_etag_changes_with_membership(self):
        first = self._get()
        PGM["+821310002001"] = (PG, 3)          # 현장A 편입
        try:
            second = self._get({"if-none-match": first.headers["ETag"]})
        finally:
            PGM["+821310002001"] = ("pg-field", 1)
        self.assertEqual(second.status, 200)
        self.assertNotEqual(second.headers["ETag"], first.headers["ETag"])
        self.assertEqual(len(second.body["phoneGroup"]["members"]), 3)
        self.assertEqual(len(second.body["dispatch"]["members"]), 3)

    def test_unmigrated_db_omits_blocks(self):
        self.cur = _FakeCursor(_role(), tables=False)
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertNotIn("dispatch", r.body)
        self.assertNotIn("phoneGroup", r.body)
        self.assertIn("ETag", r.headers)

    def test_non_dispatcher_omits_blocks(self):
        self.token = {"sub": "field01", "mcptt_id": "tel:+821310003001", "scope": [m.SCOPE_PROVISIONING]}
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertNotIn("dispatch", r.body)
        self.assertNotIn("phoneGroup", r.body)

    def test_phone_group_only_has_both_blocks_with_none_scopes(self):
        self.cur = _FakeCursor(None)
        r = self._get()
        self.assertEqual(r.body["phoneGroup"]["groupId"], PG)
        self.assertEqual((r.body["dispatch"]["monitorScope"], r.body["dispatch"]["pttListen"]), ("none", "none"))
        self.assertNotIn("roleId", r.body["dispatch"])

    def test_service_capabilities_sms_gateway(self):
        """접속서비스 능력 — smsGateway 는 csc.json Provisioning.Services.<kind>.sms_gateway(기본 false, 문자열 bool 허용)."""
        r = self._get()
        self.assertEqual([s["capabilities"] for s in r.body["services"]],
                         [{"smsGateway": False}, {"smsGateway": False}])
        m.PROVISIONING = {"Services": {"volte": {"sms_gateway": True}, "ptt": {"sms_gateway": "false"}}}
        r = self._get()
        caps = {s["kind"]: s["capabilities"]["smsGateway"] for s in r.body["services"]}
        self.assertEqual(caps, {"volte": True, "ptt": False})
        m.PROVISIONING = {"Services": {"ptt": {"sms_gateway": "true"}}}
        r = self._get()
        self.assertTrue({s["kind"]: s["capabilities"]["smsGateway"] for s in r.body["services"]}["ptt"])

    def test_service_ref_voip_selects_voip_entry(self):
        """service_ref=voip 회선은 Provisioning.Services.voip 항목으로 프로비저닝된다(와이어 kind=voip·voip 도메인).
        같은 volte_subscriptions 테이블의 이동 회선(service_ref 빈값)은 volte 그대로(sip_service_model.md §2-9)."""
        m.PROVISIONING = {"Services": {
            "volte": {"name": "volte", "domain": "volte.cims.example.kr", "port": 5060, "tls_port": 5061},
            "voip": {"name": "voip", "domain": "voip.cims.example.kr", "port": 5060, "tls_port": 15061,
                     "transport": "TLS"},
            "ptt": {"name": "mcptt", "domain": "ptt.cims.example.kr", "port": 5060, "tls_port": 5061},
        }}
        SREF["+821310001001"] = "voip"
        try:
            r = self._get()
        finally:
            SREF.clear()
        self.assertEqual(r.status, 200)
        by_kind = {s["kind"]: s for s in r.body["services"]}
        self.assertEqual(sorted(by_kind), ["ptt", "voip"])
        self.assertEqual(by_kind["voip"]["sip"]["domain"], "voip.cims.example.kr")
        self.assertEqual(by_kind["voip"]["sip"]["transport"], "TLS")
        self.assertEqual(by_kind["voip"]["sip"]["port"], 15061)
        # ptt 회선의 service_ref 는 종류 경계를 넘지 못한다 — 'voip' 를 가리켜도 ptt 항목 유지
        SREF["+82510001001"] = "voip"
        try:
            r = self._get()
        finally:
            SREF.clear()
        self.assertEqual([s["kind"] for s in r.body["services"]], ["volte", "ptt"])
        self.assertEqual(by_kind["ptt"]["sip"]["domain"], "ptt.cims.example.kr")
        # 이름 불일치(라이브 access_services.name='mcptt' vs 키 'ptt') — 종류 키 폴백
        SREF["+82510001001"] = "mcptt"
        try:
            r = self._get()
        finally:
            SREF.clear()
        self.assertEqual([s["kind"] for s in r.body["services"]], ["volte", "ptt"])


class ScopeSetsTests(_Base):
    """_dispatch_scope_sets — 이력·녹취 게이트의 역할 범위 집합(dispatch_center.md §5.7a). 역할 없음·두 범위 none → None(403)."""

    def test_sets(self):
        az.reset_probe()
        s = m._dispatch_scope_sets(_FakeCursor(_role("listed", "listed")), 5020)
        self.assertEqual((s["roleId"], s["groupId"], s["monitorCall"], s["pttListen"]), (ROLE_ID, PG, "listed", "listed"))
        self.assertEqual(s["members"], {"+821310001001", "+821310001002", "+821310002001", "+821310002002"})
        self.assertEqual(s["ptt_groups"], {"g002", "g-0a1b2c3d"})
        az.reset_probe()
        s = m._dispatch_scope_sets(_FakeCursor(_role("none", "all")), 5020)
        self.assertEqual(s["members"], set())                       # monitor_call=none — 그룹원 BLF 는 되지만 통화 이력은 아니다
        self.assertEqual(len(s["ptt_groups"]), 3)

    def test_no_scope(self):
        az.reset_probe()
        self.assertIsNone(m._dispatch_scope_sets(_FakeCursor(None), 5020))                  # 전화 그룹만
        az.reset_probe()
        self.assertIsNone(m._dispatch_scope_sets(_FakeCursor(_role("none", "none")), 5020))  # 역할은 있으나 범위 없음
        az.reset_probe()
        self.assertIsNone(m._dispatch_scope_sets(_FakeCursor(_role("all")), 5040))         # 미배정


if __name__ == "__main__":
    unittest.main()
