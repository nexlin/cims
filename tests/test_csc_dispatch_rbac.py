"""csc/src/handlers/dispatch.py — 관제 그룹 편입 RBAC 단위 테스트 (오프라인, DB 없음).

역할(role)은 콘솔 계정(토큰 클레임)에만 있고 가입자(DB users = person 전용)에는 없다
(dispatch_center.md §5.3·§5.8, mcptt_authorization.md §2). 감청/청취 그룹(monitor_scope/ptt_listen≠none)
편입은 콘솔 manager 승인 하나로 결정돼야 하며, 가입자 쪽 역할을 DB 에서 찾는 SQL(users.role —
sql/migrate_users_person_only.sql 로 DROP 된 컬럼)은 어떤 경로에서도 나가면 안 된다.

  python3 -m unittest tests.test_csc_dispatch_rbac
"""
from __future__ import annotations

import importlib.util
import os
import sys
import types
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_CSC_SRC = os.path.join(_REPO_ROOT, "csc", "src")


def _load_dispatch():
    """handlers/dispatch.py 를 파일 경로로 적재 — services.mcptt(CSP 통지·XCAP)는 스텁으로 대체."""
    if _CSC_SRC not in sys.path:
        sys.path.insert(0, _CSC_SRC)
    for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
        if os.path.isdir(_v) and _v not in sys.path:
            sys.path.append(_v)
            break
    import services  # noqa: F401  — 실제 패키지 (admin_auth 는 그대로 사용)
    if "services.mcptt" not in sys.modules:
        stub = types.ModuleType("services.mcptt")
        stub.notify_csp = lambda *a, **k: None
        sys.modules["services.mcptt"] = stub
    spec = importlib.util.spec_from_file_location(
        "handlers_dispatch_under_test", os.path.join(_CSC_SRC, "handlers", "dispatch.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FakeCursor:
    """dispatch.py 가 내는 SQL 만 흉내 내는 DictCursor. 실행 SQL 을 전부 기록한다.

    subscribers: 회선 id 집합(전부 volte 로 취급) 또는 {회선 id: (table, person_id)} — person 단위 파생 시험용.
    회선별 pickup_group 과 멤버 행(user_id → (group_id, alert_order))을 들고 INSERT/UPDATE/DELETE 를 반영한다."""

    def __init__(self, group_row: dict, subscribers):
        self.group_row = dict(group_row)
        if isinstance(subscribers, dict):
            self.lines = {k: {"table": v[0], "person": v[1]} for k, v in subscribers.items()}
        else:
            self.lines = {k: {"table": "volte_subscriptions", "person": i + 1} for i, k in enumerate(sorted(subscribers))}
        self.pickup = {k: None for k in self.lines}
        self.members: dict[str, tuple[str, int]] = {}
        self.executed: list[tuple[str, tuple]] = []
        self._next = None
        self._rows = []
        self.rowcount = 0

    def _line_ids(self, table, person):
        return sorted(k for k, v in self.lines.items() if v["table"] == table and v["person"] == person)

    def execute(self, sql, params=None):
        s = " ".join(sql.split())
        p = tuple(params) if params else ()
        self.executed.append((s, p))
        self._next = None
        self._rows = []
        self.rowcount = 0
        if s.startswith("SHOW TABLES LIKE 'dispatch_groups'"):
            self._next = {"Tables_in_cims": "dispatch_groups"}
        elif s.startswith("SELECT monitor_scope, ptt_listen FROM dispatch_groups WHERE id="):
            self._next = dict(self.group_row)
        elif s.startswith("SELECT 1 FROM volte_subscriptions WHERE id=") or \
                s.startswith("SELECT 1 FROM ptt_subscriptions WHERE id="):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            self._next = {"1": 1} if p and p[0] in self.lines and self.lines[p[0]]["table"] == t else None
        elif s.startswith("SELECT user_id FROM volte_subscriptions WHERE id=") or \
                s.startswith("SELECT user_id FROM ptt_subscriptions WHERE id="):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            ln = self.lines.get(p[0]) if p else None
            self._next = {"user_id": ln["person"]} if ln and ln["table"] == t else None
        elif s.startswith("SELECT group_id FROM dispatch_group_members WHERE user_id="):
            m = self.members.get(p[0]) if p else None
            self._next = {"group_id": m[0]} if m else None
        elif s.startswith("SELECT m.group_id FROM dispatch_group_members m WHERE m.user_id IN ("):
            person = p[0]
            cands = sorted(((g, o, uid) for uid, (g, o) in self.members.items()
                            if self.lines.get(uid, {}).get("person") == person), key=lambda x: (x[1], x[2]))
            self._next = {"group_id": cands[0][0]} if cands else None
        elif s.startswith("SELECT id, pickup_group FROM volte_subscriptions WHERE") or \
                s.startswith("SELECT id, pickup_group FROM ptt_subscriptions WHERE"):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            ids = [p[0]] if "WHERE id=" in s else self._line_ids(t, p[0])
            self._rows = [{"id": i, "pickup_group": self.pickup.get(i)} for i in ids if i in self.lines
                          and self.lines[i]["table"] == t]
        elif s.startswith("SELECT user_id FROM dispatch_group_members WHERE group_id="):
            self._rows = [{"user_id": uid} for uid, (g, _) in sorted(self.members.items()) if g == p[0]]
        elif s.startswith("SHOW COLUMNS FROM"):
            self._next = {"Field": "pickup_group"}
        elif s.startswith("SELECT 1 FROM dispatch_groups WHERE id=") or \
                s.startswith("SELECT id FROM dispatch_groups WHERE pilot_id="):
            self._next = None
        elif s.startswith("INSERT INTO dispatch_group_members"):
            self.members[p[0]] = (p[1], p[2])
            self.rowcount = 1
        elif s.startswith("DELETE FROM dispatch_group_members WHERE group_id="):
            self.rowcount = 1 if self.members.pop(p[1], None) else 0
        elif s.startswith("DELETE FROM dispatch_groups WHERE id="):
            for uid in [u for u, (g, _) in self.members.items() if g == p[0]]:   # FK CASCADE
                del self.members[uid]
            self.rowcount = 1
        elif s.startswith("UPDATE volte_subscriptions SET pickup_group=") or \
                s.startswith("UPDATE ptt_subscriptions SET pickup_group="):
            self.pickup[p[1]] = p[0]
            self.rowcount = 1
        elif s.startswith("INSERT") or s.startswith("UPDATE"):
            self.rowcount = 1
        elif "users" in s.lower():
            # 가입자 역할 조회 — 컬럼이 없어 운영 DB 에서는 1054 로 터진다. 시험에서는 그대로 실패시킨다.
            raise AssertionError(f"users 테이블 조회는 허용되지 않는다: {s}")

    def fetchone(self):
        r, self._next = self._next, None
        return r

    def fetchall(self):
        r, self._rows = self._rows, []
        return r


class DispatchRbacTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = _load_dispatch()
        cls.d._HAS_TABLES = True
        cls.notified = []
        cls.d.notify_csp = lambda *a, **k: cls.notified.append(a)

    def _no_role_sql(self, cur: FakeCursor):
        for s, _ in cur.executed:
            low = s.lower()
            self.assertNotIn(".role", low, s)
            self.assertNotIn("join users", low, s)
            self.assertNotIn("from users", low, s)

    # ── POST /dispatch-groups/{id}/members ──
    def test_add_member_monitoring_group_manager_201(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "all"}, {"+82310001001"})
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82310001001", "alert_order": 0}, is_manager=True)
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(r.body["user_id"], "+82310001001")
        self._no_role_sql(cur)
        self.assertTrue(any(s.startswith("INSERT INTO dispatch_group_members") for s, _ in cur.executed))
        self.assertTrue(any(s.startswith("UPDATE volte_subscriptions SET pickup_group=") for s, _ in cur.executed))

    def test_add_member_monitoring_group_operator_403(self):
        cur = FakeCursor({"monitor_scope": "own", "ptt_listen": "none"}, {"+82310001001"})
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82310001001"}, is_manager=False)
        self.assertEqual(r.status, 403)
        self.assertEqual(r.body["error"], "manager_required")
        self.assertFalse(any(s.startswith("INSERT") for s, _ in cur.executed))

    def test_add_member_ptt_listen_group_operator_403(self):
        cur = FakeCursor({"monitor_scope": "none", "ptt_listen": "listed"}, {"+82310001001"})
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82310001001"}, is_manager=False)
        self.assertEqual(r.status, 403)
        self.assertEqual(r.body["error"], "manager_required")

    def test_add_member_plain_group_operator_201(self):
        cur = FakeCursor({"monitor_scope": "none", "ptt_listen": "none"}, {"+82310001001"})
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82310001001"}, is_manager=False)
        self.assertEqual(r.status, 201, r.body)
        self._no_role_sql(cur)

    def test_add_member_unknown_subscriber_404(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "none"}, set())
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82310009999"}, is_manager=True)
        self.assertEqual(r.status, 404)

    # ── POST /dispatch-groups (inline members) — 같은 게이트 하나로 ──
    def test_create_monitoring_group_inline_members_manager_201(self):
        cur = FakeCursor({}, {"+82310001001", "+82310001002"})
        body = {"id": "dg-t2", "name": "관제", "monitor_scope": "all", "ptt_listen": "all",
                "members": [{"user_id": "+82310001001", "alert_order": 0}, {"user_id": "+82310001002", "alert_order": 1}]}
        r = self.d._create_group(cur, body, is_manager=True)
        self.assertEqual(r.status, 201, r.body)
        self._no_role_sql(cur)
        self.assertEqual(sum(1 for s, _ in cur.executed if s.startswith("INSERT INTO dispatch_group_members")), 2)

    def test_create_monitoring_group_operator_403(self):
        cur = FakeCursor({}, {"+82310001001"})
        r = self.d._create_group(cur, {"name": "관제", "monitor_scope": "own"}, is_manager=False)
        self.assertEqual(r.status, 403)
        self.assertEqual(r.body["error"], "manager_required")

    # ── pickup_group 파생 = person 단위 (관제사 VoLTE 멤버 → 같은 사람의 PTT 회선, §3.2·§5.6) ──
    #    관제사 = person 1: VoLTE +821310001001 (멤버 행) + PTT +82510001001 (멤버 아님 — 대표번호 포크 대상이 되면 안 된다)
    _DISPATCHER = {"+821310001001": ("volte_subscriptions", 1), "+82510001001": ("ptt_subscriptions", 1),
                   "+821310001002": ("volte_subscriptions", 2), "+82510001002": ("ptt_subscriptions", 2)}

    def _notify_targets(self):
        return [a[1] for a in self.notified if a[0] == "USER_CHANGED"]

    def test_add_member_derives_ptt_line_of_same_person(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "all"}, self._DISPATCHER)
        self.notified.clear()
        r = self.d._add_member(cur, "dg-t", {"user_id": "+821310001001", "alert_order": 0}, is_manager=True)
        self.assertEqual(r.status, 201, r.body)
        self._no_role_sql(cur)
        self.assertEqual(set(cur.members), {"+821310001001"})                     # 멤버 행은 VoLTE 회선만
        self.assertEqual(cur.pickup["+821310001001"], "dg-t")
        self.assertEqual(cur.pickup["+82510001001"], "dg-t")                       # PTT 회선이 파생으로 물려받음
        self.assertIsNone(cur.pickup["+821310001002"])                             # 다른 person 은 무관
        self.assertIsNone(cur.pickup["+82510001002"])
        self.assertEqual(self._notify_targets(), ["tel:+821310001001", "tel:+82510001001"])  # CSP 회선별 캐시 재적재
        self.assertEqual(self.d.effective_dispatch_group(cur, "+82510001001"), "dg-t")
        self.assertIsNone(self.d.dispatch_group_of_user(cur, "+82510001001"))     # 멤버십 자체는 없음

    def test_remove_member_clears_derived_ptt_line(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "all"}, self._DISPATCHER)
        self.d._add_member(cur, "dg-t", {"user_id": "+821310001001"}, is_manager=True)
        self.notified.clear()
        r = self.d._remove_member(cur, "dg-t", "+821310001001")
        self.assertEqual(r.status, 200, r.body)
        self.assertIsNone(cur.pickup["+821310001001"])
        self.assertIsNone(cur.pickup["+82510001001"])
        self.assertEqual(self._notify_targets(), ["tel:+821310001001", "tel:+82510001001"])
        self.assertIsNone(self.d.effective_dispatch_group(cur, "+82510001001"))

    def test_delete_group_clears_derived_lines_of_all_members(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "all"}, self._DISPATCHER)
        self.d._add_member(cur, "dg-t", {"user_id": "+821310001001", "alert_order": 0}, is_manager=True)
        self.d._add_member(cur, "dg-t", {"user_id": "+821310001002", "alert_order": 1}, is_manager=True)
        self.assertEqual(cur.pickup["+82510001002"], "dg-t")
        self.notified.clear()
        r = self.d._delete_group(cur, "dg-t")
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(cur.members, {})
        self.assertTrue(all(v is None for v in cur.pickup.values()), cur.pickup)
        self.assertEqual(set(self._notify_targets()),
                         {"tel:+821310001001", "tel:+82510001001", "tel:+821310001002", "tel:+82510001002"})

    def test_ptt_only_member_keeps_own_membership(self):
        # S3-SCN-PTT-LISTEN 픽스처처럼 PTT 회선 자체를 멤버로 넣는 경우 — 자기 멤버십이 파생보다 우선
        cur = FakeCursor({"monitor_scope": "none", "ptt_listen": "all"}, {"+82510009001": ("ptt_subscriptions", 9)})
        r = self.d._add_member(cur, "dg-t", {"user_id": "+82510009001"}, is_manager=True)
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(cur.pickup["+82510009001"], "dg-t")
        self.assertEqual(self.d.dispatch_group_of_user(cur, "+82510009001"), "dg-t")
        self.assertEqual(self.d.effective_dispatch_group(cur, "+82510009001"), "dg-t")

    def test_sync_is_idempotent_no_rewrite_when_unchanged(self):
        cur = FakeCursor({"monitor_scope": "all", "ptt_listen": "all"}, self._DISPATCHER)
        self.d._add_member(cur, "dg-t", {"user_id": "+821310001001"}, is_manager=True)
        n_upd = sum(1 for s, _ in cur.executed if s.startswith("UPDATE"))
        self.assertEqual(self.d._sync_pickup_group(cur, "+821310001001"), [])     # 값 동일 → 쓰기 없음
        self.assertEqual(sum(1 for s, _ in cur.executed if s.startswith("UPDATE")), n_upd)

    # ── 소스 정적 확인 — 가입자 역할 게이트 잔재 없음 ──
    def test_source_has_no_subscriber_role_gate(self):
        with open(os.path.join(_CSC_SRC, "handlers", "dispatch.py"), encoding="utf-8") as f:
            src = f.read()
        self.assertNotIn("_member_role_ok", src)
        self.assertNotIn("member_role_insufficient", src)
        self.assertNotIn("u.role", src)


if __name__ == "__main__":
    unittest.main()
