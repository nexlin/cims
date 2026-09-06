"""csc/src/services/mcptt.py — /provisioning/me 관제 데스크 발견(discovery) 단위 시험 (오프라인, DB 없음).

dispatch_center.md §8.4 / android_ue_provisioning.md §3: `dispatch` 블록의 members[](CSP CanWatch 와 같은 규칙으로
monitor_scope 를 해석한 VoLTE 가입자)·pttTargets[](CanListenPtt 와 같은 규칙으로 ptt_listen 을 해석한 PTT 그룹)·
etag, 응답 ETag + If-None-Match 304. 가짜 커서가 dispatch_discovery 가 내는 SQL 만 흉내 낸다.

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
from httpsrv.handler import HandlerArgs  # noqa: E402

DG = "dg-dispatch01"
# users: id → (name)
USERS = {5020: "관제1석", 5021: "관제2석", 5030: "현장A", 5031: "현장B", 5040: "타부서"}
# volte_subscriptions: msisdn → user_id
VOLTE = {"+821310001001": 5020, "+821310001002": 5021, "+821310002001": 5030, "+821310002002": 5031,
         "+821310003001": 5040}
# ptt_subscriptions: msisdn → user_id (관제2석은 PTT 미가입, 현장A 는 PTT 2회선)
PTT = {"+82510001001": 5020, "+82510002001": 5030, "+82510002009": 5030, "+82510002002": 5031}
# dispatch_group_members: volte msisdn → (group_id, alert_order)
DGM = {"+821310001002": (DG, 2), "+821310001001": (DG, 1),
       "+821310002001": ("dg-field", 1), "+821310002002": ("dg-field", 2)}
MONITOR_TARGETS = {DG: {"dg-field"}}
PTT_GROUPS = [(24, "g002", "음성그룹2"), (25, "g001", "음성그룹1"), (30, "g-0a1b2c3d", "관제임시")]
PTT_TARGETS = {DG: {24, 30}}


class _FakeCursor:
    """dispatch_discovery / handle_provisioning_me 가 내는 SQL 만 흉내 내는 tuple 커서."""

    def __init__(self, group: dict | None, dispatch_tables: bool = True):
        self.group = group            # dispatch_groups 행 (없으면 미소속)
        self.dispatch_tables = dispatch_tables
        self.sql: list[tuple[str, tuple]] = []
        self._rows: list = []

    # ── 도우미 ──
    def _own_group_row(self, user_id):
        if not self.group:
            return None
        for vid, (gid, _o) in DGM.items():
            if VOLTE.get(vid) == user_id and gid == self.group["id"]:
                g = self.group
                return (g["id"], g["name"], g.get("pilot_id") or "", g["monitor_scope"], g["ptt_listen"],
                        g["listen_visibility"])
        return None

    def _member_rows(self, where: str, gid: str):
        rows = []
        for vid, uid in VOLTE.items():
            mg, order = DGM.get(vid, ("", 0))
            if where == "own" and mg != gid:
                continue
            if where == "listed" and not (mg == gid or mg in MONITOR_TARGETS.get(gid, set())):
                continue
            ptt_ids = sorted(p for p, u in PTT.items() if u == uid)
            rows.append((uid, USERS[uid], vid, mg, ptt_ids[0] if ptt_ids else None, order))
        # ORDER BY CASE WHEN m.group_id=gid THEN 0 ELSE 1 END, m.group_id, m.alert_order, s.id
        rows.sort(key=lambda r: (0 if r[3] == gid else 1, r[3], r[5], r[2]))
        return [r[:5] for r in rows]

    # ── DB-API ──
    def execute(self, q, args=None):
        self.sql.append((q, tuple(args) if args is not None else ()))
        self._rows = []
        if "dispatch_group" in q and not self.dispatch_tables:
            raise RuntimeError("(1146, \"Table 'cims.dispatch_group_members' doesn't exist\")")
        if q.startswith("SELECT user_id FROM volte_subscriptions WHERE id="):
            uid = VOLTE.get(args[0])
            self._rows = [(uid,)] if uid is not None else []
        elif q.startswith("SELECT user_id FROM ptt_subscriptions WHERE id="):
            uid = PTT.get(args[0])
            self._rows = [(uid,)] if uid is not None else []
        elif q.startswith("SELECT id, imsi, auth_id, sip_transport, ha1, auth_scheme") and "volte_subscriptions" in q:
            self._rows = [(vid, "45033" + vid[-10:], "", "TLS", "0" * 32, "digest", "", "", "")
                          for vid, uid in sorted(VOLTE.items()) if uid == args[0]]
        elif q.startswith("SELECT id, imsi, auth_id, sip_transport, ha1, auth_scheme") and "ptt_subscriptions" in q:
            self._rows = [(pid, "45033" + pid[-10:], "", "TLS", "0" * 32, "digest", "", "", "")
                          for pid, uid in sorted(PTT.items()) if uid == args[0]]
        elif q.startswith("SELECT name FROM users WHERE id="):
            self._rows = [(USERS[args[0]],)] if args[0] in USERS else []
        elif q.startswith("SELECT g.id, g.name, COALESCE(g.pilot_id,''), g.monitor_scope"):
            r = self._own_group_row(args[0])
            self._rows = [r] if r else []
        elif q.startswith("SELECT u.id, u.name, s.id, COALESCE(m.group_id,'')"):
            if "OR m.group_id IN" in q:
                self._rows = self._member_rows("listed", args[0])
            elif " WHERE m.group_id=%s" in q:
                self._rows = self._member_rows("own", args[0])
            else:
                self._rows = self._member_rows("all", args[0])
        elif q.startswith("SELECT mcptt_group_id, name FROM ptt_groups"):
            self._rows = sorted(((g, n) for _pk, g, n in PTT_GROUPS))
        elif q.startswith("SELECT g.mcptt_group_id, g.name FROM dispatch_group_ptt_targets"):
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


def _group(scope="own", ptt_listen="none", vis="hidden"):
    return {"id": DG, "name": "관제 1조", "pilot_id": "+821310001000", "monitor_scope": scope,
            "ptt_listen": ptt_listen, "listen_visibility": vis}


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self._prov = m.PROVISIONING
        m.PROVISIONING = {}

    def tearDown(self):
        m.PROVISIONING = self._prov

    def test_not_a_dispatcher_returns_none(self):
        cur = _FakeCursor(_group())
        self.assertIsNone(m.dispatch_discovery(cur, 5040))

    def test_scope_own_members_are_own_group_in_alert_order(self):
        cur = _FakeCursor(_group("own"))
        d = m.dispatch_discovery(cur, 5021)
        self.assertEqual(d["groupId"], DG)
        self.assertEqual(d["pilotId"], "+821310001000")
        self.assertEqual([x["volteAor"] for x in d["members"]], ["tel:+821310001001", "tel:+821310001002"])
        self.assertTrue(all(x["groupId"] == DG for x in d["members"]))
        self.assertEqual(d["pttTargets"], [])
        # 자기 자신 포함, userId=users.id(person), 내선=E.164 끝 4자리
        me = d["members"][1]
        self.assertEqual((me["userId"], me["name"], me["extension"]), (5021, "관제2석", "1002"))

    def test_scope_none_still_lists_own_group(self):
        """CanWatch 규칙 1 — 같은 픽업 그룹은 monitor_scope 와 무관하게 허용."""
        d = m.dispatch_discovery(_FakeCursor(_group("none")), 5020)
        self.assertEqual(len(d["members"]), 2)
        self.assertEqual(d["monitorScope"], "none")

    def test_ptt_id_first_subscription_or_empty(self):
        d = m.dispatch_discovery(_FakeCursor(_group("listed")), 5020)
        by_uid = {x["userId"]: x for x in d["members"]}
        self.assertEqual(by_uid[5020]["pttId"], "tel:+82510001001")
        self.assertEqual(by_uid[5021]["pttId"], "")                    # PTT 미가입
        self.assertEqual(by_uid[5030]["pttId"], "tel:+82510002001")   # 2회선 → MIN(id)

    def test_scope_listed_adds_target_group_members_after_own(self):
        d = m.dispatch_discovery(_FakeCursor(_group("listed")), 5020)
        self.assertEqual([x["groupId"] for x in d["members"]], [DG, DG, "dg-field", "dg-field"])
        self.assertNotIn(5040, [x["userId"] for x in d["members"]])   # 무소속 가입자는 listed 범위 밖

    def test_scope_all_is_every_volte_subscriber_like_csp_canwatch(self):
        d = m.dispatch_discovery(_FakeCursor(_group("all")), 5020)
        self.assertEqual(len(d["members"]), len(VOLTE))
        self.assertEqual([x["groupId"] for x in d["members"][:2]], [DG, DG])   # 자기 그룹 먼저
        by_uid = {x["userId"]: x for x in d["members"]}
        self.assertEqual(by_uid[5040]["groupId"], "")                  # 관제 그룹 없는 가입자 groupId=""
        self.assertEqual(by_uid[5030]["groupId"], "dg-field")

    def test_ptt_targets_listed_and_all_use_tel_uri(self):
        d = m.dispatch_discovery(_FakeCursor(_group(ptt_listen="listed")), 5020)
        self.assertEqual(d["pttTargets"], [{"id": "g-0a1b2c3d", "uri": "tel:g-0a1b2c3d", "name": "관제임시"},
                                           {"id": "g002", "uri": "tel:g002", "name": "음성그룹2"}])
        d = m.dispatch_discovery(_FakeCursor(_group(ptt_listen="all")), 5020)
        self.assertEqual([t["id"] for t in d["pttTargets"]], ["g-0a1b2c3d", "g001", "g002"])

    def test_extension_digits_setting(self):
        m.PROVISIONING = {"ExtensionDigits": 3}
        d = m.dispatch_discovery(_FakeCursor(_group()), 5020)
        self.assertEqual(d["members"][0]["extension"], "001")
        m.PROVISIONING = {"ExtensionDigits": 0}
        d = m.dispatch_discovery(_FakeCursor(_group()), 5020)
        self.assertEqual(d["members"][0]["extension"], "821310001001")
        m.PROVISIONING = {"ExtensionDigits": "bogus"}
        d = m.dispatch_discovery(_FakeCursor(_group()), 5020)
        self.assertEqual(d["members"][0]["extension"], "1001")

    def test_block_etag_is_content_derived(self):
        a = m.dispatch_discovery(_FakeCursor(_group("own")), 5020)
        b = m.dispatch_discovery(_FakeCursor(_group("own")), 5020)
        c = m.dispatch_discovery(_FakeCursor(_group("all")), 5020)
        self.assertEqual(a["etag"], b["etag"])
        self.assertNotEqual(a["etag"], c["etag"])
        self.assertRegex(a["etag"], r'^"[0-9a-f]{32}"$')
        # etag 는 자기 자신을 제외한 내용의 해시
        probe = {k: v for k, v in a.items() if k != "etag"}
        self.assertEqual(m._content_etag_json(probe), a["etag"])

    def test_sql_shapes(self):
        """CSP 와 같은 규칙임을 SQL 로 고정 — listed 는 monitor_targets 서브쿼리, all 은 WHERE 없음."""
        cur = _FakeCursor(_group("listed", "listed"))
        m.dispatch_discovery(cur, 5020)
        member_q = [q for q, _ in cur.sql if q.startswith("SELECT u.id, u.name")][0]
        self.assertIn("dispatch_group_monitor_targets WHERE group_id=%s", member_q)
        self.assertIn("ORDER BY CASE WHEN m.group_id=%s THEN 0 ELSE 1 END", member_q)
        self.assertTrue(any(q.startswith("SELECT g.mcptt_group_id, g.name FROM dispatch_group_ptt_targets")
                            for q, _ in cur.sql))
        cur = _FakeCursor(_group("all", "all"))
        m.dispatch_discovery(cur, 5020)
        member_q = [q for q, _ in cur.sql if q.startswith("SELECT u.id, u.name")][0]
        self.assertNotIn(" WHERE m.group_id", member_q)                 # 바깥 WHERE 없음(서브쿼리 WHERE 만)
        self.assertTrue(any(q == "SELECT mcptt_group_id, name FROM ptt_groups ORDER BY mcptt_group_id" for q, _ in cur.sql))


class HandlerTests(unittest.TestCase):
    """handle_provisioning_me — 블록 탑재·ETag·If-None-Match 304·테이블 미적용 생략."""

    def setUp(self):
        self._saved = (m._DB_CONFIG, m.extract_token, m.PROVISIONING, sys.modules.get("pymysql"))
        m._DB_CONFIG = {"Host": "127.0.0.1", "Port": 3306, "User": "cims", "Password": "", "Db": "cims"}
        m.PROVISIONING = {}
        self.token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
        m.extract_token = lambda hdr: self.token if hdr else None
        self.cur = _FakeCursor(_group("own", "listed"))
        sys.modules["pymysql"] = types.SimpleNamespace(connect=lambda **kw: _FakeConn(self.cur))

    def tearDown(self):
        m._DB_CONFIG, m.extract_token, m.PROVISIONING, pm = self._saved
        if pm is None:
            sys.modules.pop("pymysql", None)
        else:
            sys.modules["pymysql"] = pm

    def _get(self, headers=None):
        h = {"authorization": "Bearer x", "host": "csc.test:4430"}
        h.update(headers or {})
        return asyncio.run(m.handle_provisioning_me(HandlerArgs("GET", "/provisioning/me", "127.0.0.1", 0, headers=h), {}))

    def test_dispatch_block_and_etag_header(self):
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertRegex(r.headers.get("ETag", ""), r'^"[0-9a-f]{32}"$')
        d = r.body["dispatch"]
        self.assertEqual(d["groupId"], DG)
        self.assertEqual([x["extension"] for x in d["members"]], ["1001", "1002"])
        self.assertEqual([t["uri"] for t in d["pttTargets"]], ["tel:g-0a1b2c3d", "tel:g002"])
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
        DGM["+821310002001"] = (DG, 3)          # 현장A 편입
        try:
            second = self._get({"if-none-match": first.headers["ETag"]})
        finally:
            DGM["+821310002001"] = ("dg-field", 1)
        self.assertEqual(second.status, 200)
        self.assertNotEqual(second.headers["ETag"], first.headers["ETag"])
        self.assertEqual(len(second.body["dispatch"]["members"]), 3)

    def test_unmigrated_db_omits_block(self):
        self.cur = _FakeCursor(_group(), dispatch_tables=False)
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertNotIn("dispatch", r.body)
        self.assertIn("ETag", r.headers)

    def test_non_dispatcher_omits_block(self):
        self.token = {"sub": "field01", "mcptt_id": "tel:+821310003001", "scope": [m.SCOPE_PROVISIONING]}
        r = self._get()
        self.assertEqual(r.status, 200)
        self.assertNotIn("dispatch", r.body)


if __name__ == "__main__":
    unittest.main()
