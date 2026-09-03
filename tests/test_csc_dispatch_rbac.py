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
    """dispatch.py 가 내는 SQL 만 흉내 내는 DictCursor. 실행 SQL 을 전부 기록한다."""

    def __init__(self, group_row: dict, subscribers: set[str]):
        self.group_row = dict(group_row)
        self.subscribers = set(subscribers)
        self.executed: list[tuple[str, tuple]] = []
        self._next = None
        self.rowcount = 0

    def execute(self, sql, params=None):
        s = " ".join(sql.split())
        p = tuple(params) if params else ()
        self.executed.append((s, p))
        self._next = None
        self.rowcount = 0
        if s.startswith("SHOW TABLES LIKE 'dispatch_groups'"):
            self._next = {"Tables_in_cims": "dispatch_groups"}
        elif s.startswith("SELECT monitor_scope, ptt_listen FROM dispatch_groups WHERE id="):
            self._next = dict(self.group_row)
        elif s.startswith("SELECT 1 FROM volte_subscriptions WHERE id="):
            self._next = {"1": 1} if p and p[0] in self.subscribers else None
        elif s.startswith("SELECT 1 FROM ptt_subscriptions WHERE id="):
            self._next = None
        elif s.startswith("SELECT group_id FROM dispatch_group_members WHERE user_id="):
            self._next = None
        elif s.startswith("SHOW COLUMNS FROM"):
            self._next = {"Field": "pickup_group"}
        elif s.startswith("SELECT 1 FROM dispatch_groups WHERE id=") or \
                s.startswith("SELECT id FROM dispatch_groups WHERE pilot_id="):
            self._next = None
        elif s.startswith("INSERT") or s.startswith("UPDATE"):
            self.rowcount = 1
        elif "users" in s.lower():
            # 가입자 역할 조회 — 컬럼이 없어 운영 DB 에서는 1054 로 터진다. 시험에서는 그대로 실패시킨다.
            raise AssertionError(f"users 테이블 조회는 허용되지 않는다: {s}")

    def fetchone(self):
        r, self._next = self._next, None
        return r

    def fetchall(self):
        return []


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

    # ── 소스 정적 확인 — 가입자 역할 게이트 잔재 없음 ──
    def test_source_has_no_subscriber_role_gate(self):
        with open(os.path.join(_CSC_SRC, "handlers", "dispatch.py"), encoding="utf-8") as f:
            src = f.read()
        self.assertNotIn("_member_role_ok", src)
        self.assertNotIn("member_role_insufficient", src)
        self.assertNotIn("u.role", src)


if __name__ == "__main__":
    unittest.main()
