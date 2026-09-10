"""csc/src/handlers/dispatch.py — 전화 그룹 멤버십·pickup_group 파생 + 역할 배정·청취 자격 동기 단위 테스트 (오프라인, DB 없음).

dispatch_center.md §3.1~§3.3 · mcptt_authorization.md §2.4·§3 · admin_api.md §6.7·§6.8:
  - 전화 그룹 멤버십은 권한이 아니다 — 편입에 역할 게이트가 없고 가입자(DB users = person 전용) 쪽 역할 SQL 도 나가지 않는다.
  - pickup_group 파생은 person 단위(관제사 VoLTE 멤버 → 같은 사람의 PTT 회선), 값이 바뀐 회선마다 USER_CHANGED.
  - 범위(directory_write=own) 는 그룹 org 와 멤버 person 의 org 로 판정(403 out_of_scope).
  - 역할: 커스텀에 위임 불가 능력 400 not_delegable · 내장 읽기 전용 403 builtin · 배정 남은 삭제 409 assigned ·
    배정/해제 시 ptt_user_profile.allow_ambient_listening 동기 + ROLE_CHANGED.

  python3 -m unittest tests.test_csc_dispatch_rbac
"""
from __future__ import annotations

import importlib.util
import os
import re
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
    import services  # noqa: F401  — 실제 패키지 (admin_auth·authz 는 그대로 사용)
    if "services.mcptt" not in sys.modules:
        stub = types.ModuleType("services.mcptt")
        stub.notify_csp = lambda *a, **k: None
        sys.modules["services.mcptt"] = stub
    spec = importlib.util.spec_from_file_location(
        "handlers_dispatch_under_test", os.path.join(_CSC_SRC, "handlers", "dispatch.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ORGS = {1: "CORP", 2: "TEAM01", 3: "TEAM02"}          # organizations.id → code

ROLE_COLS = ('id', 'name', 'builtin', 'authz_manage', 'audit_read', 'directory_write', 'directory_read',
             'ptt_group_manage', 'monitor_call', 'ptt_listen', 'listen_visibility', 'history_read',
             'alarm_ack', 'mcptt_control', 'org_id')


def _role(id_, **kw):
    r = {'id': id_, 'name': id_, 'builtin': 0, 'authz_manage': 0, 'audit_read': 0, 'directory_write': 'none',
         'directory_read': 'none', 'ptt_group_manage': 'none', 'monitor_call': 'none', 'ptt_listen': 'none',
         'listen_visibility': 'hidden', 'history_read': 'none', 'alarm_ack': 0, 'mcptt_control': 0, 'org_id': None,
         'created_at': None}
    r.update(kw)
    return r


class FakeCursor:
    """dispatch.py·services/authz 가 내는 SQL 만 흉내 내는 DictCursor(작은 인메모리 DB). 실행 SQL 을 전부 기록한다.

    subscribers: 회선 id 집합(전부 volte 로 취급) 또는 {회선 id: (table, person_id[, org_code])} — person 단위 파생 시험용.
    groups: {group_id: org_id}. roles: {role_id: 행}. 회선별 pickup_group·멤버 행·배정·프로파일을 들고 쓰기를 반영한다."""

    def __init__(self, subscribers, groups=None, roles=None, users=None):
        if isinstance(subscribers, dict):
            self.lines = {k: {"table": v[0], "person": v[1], "org": (v[2] if len(v) > 2 else "")} for k, v in subscribers.items()}
        else:
            self.lines = {k: {"table": "volte_subscriptions", "person": i + 1, "org": ""} for i, k in enumerate(sorted(subscribers))}
        self.users = dict(users or {})                       # person_id → name
        for ln in self.lines.values():
            self.users.setdefault(ln["person"], f"p{ln['person']}")
        self.user_org = {ln["person"]: ln["org"] for ln in self.lines.values()}
        self.groups = dict(groups or {"pg-t": None})         # group_id → org_id
        self.roles = {k: _role(k, **v) if not isinstance(v, dict) or 'id' not in v else v for k, v in (roles or {}).items()}
        self.pickup = {k: None for k in self.lines}
        self.members: dict[str, tuple[str, int]] = {}
        self.assignments: dict[str, str] = {}                # principal_id → role_id (user)
        self.monitor_targets: set[tuple[str, str]] = set()
        self.ptt_targets: set[tuple[str, int]] = set()
        self.profiles: dict[str, int] = {}                   # ptt msisdn → allow_ambient_listening
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
        low = s.lower()
        if s.startswith("SHOW TABLES LIKE 'phone_groups'") or s.startswith("SHOW TABLES LIKE 'roles'"):
            self._next = {"Tables_in_cims": "x"}
        elif s.startswith("SHOW COLUMNS FROM"):
            self._next = {"Field": "x"}
        elif s.startswith("SELECT 1 FROM volte_subscriptions WHERE id=") or s.startswith("SELECT 1 FROM ptt_subscriptions WHERE id="):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            self._next = {"1": 1} if p and p[0] in self.lines and self.lines[p[0]]["table"] == t else None
        elif s.startswith("SELECT user_id FROM volte_subscriptions WHERE id=") or s.startswith("SELECT user_id FROM ptt_subscriptions WHERE id="):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            ln = self.lines.get(p[0]) if p else None
            self._next = {"user_id": ln["person"]} if ln and ln["table"] == t else None
        elif s.startswith("SELECT group_id FROM phone_group_members WHERE user_id="):
            m = self.members.get(p[0]) if p else None
            self._next = {"group_id": m[0]} if m else None
        elif s.startswith("SELECT m.group_id FROM phone_group_members m WHERE m.user_id IN ("):
            person = p[0]
            cands = sorted(((g, o, uid) for uid, (g, o) in self.members.items()
                            if self.lines.get(uid, {}).get("person") == person), key=lambda x: (x[1], x[2]))
            self._next = {"group_id": cands[0][0]} if cands else None
        elif s.startswith("SELECT id, pickup_group FROM volte_subscriptions WHERE") or s.startswith("SELECT id, pickup_group FROM ptt_subscriptions WHERE"):
            t = "volte_subscriptions" if "volte" in s else "ptt_subscriptions"
            ids = [p[0]] if "WHERE id=" in s else self._line_ids(t, p[0])
            self._rows = [{"id": i, "pickup_group": self.pickup.get(i)} for i in ids if i in self.lines and self.lines[i]["table"] == t]
        elif s.startswith("SELECT user_id FROM phone_group_members WHERE group_id="):
            self._rows = [{"user_id": uid} for uid, (g, _) in sorted(self.members.items()) if g == p[0]]
        elif s.startswith("SELECT user_id, alert_order FROM phone_group_members WHERE group_id="):
            self._rows = [{"user_id": uid, "alert_order": o} for uid, (g, o) in sorted(self.members.items()) if g == p[0]]
        elif s.startswith("SELECT id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, overflow_target, org_id, created_at FROM phone_groups WHERE id="):
            if p[0] in self.groups:
                self._next = {"id": p[0], "name": p[0], "pilot_id": None, "service_ref": None, "alert_mode": "parallel",
                              "no_answer_sec": 30, "busy_members": "skip", "overflow_target": None, "org_id": self.groups[p[0]], "created_at": None}
        elif s.startswith("SELECT 1 FROM phone_groups WHERE id="):
            self._next = {"1": 1} if p[0] in self.groups else None
        elif s.startswith("SELECT id FROM phone_groups WHERE pilot_id="):
            self._next = None
        elif s.startswith("SELECT code FROM organizations WHERE id="):
            self._next = {"code": ORGS[p[0]]} if p[0] in ORGS else None
        elif s.startswith("SELECT org_id FROM users WHERE id="):
            self._next = {"org_id": self.user_org.get(p[0], "")} if p[0] in self.users else None
        elif s.startswith("SELECT id, name FROM users WHERE id="):
            try:
                pid = int(p[0])
            except (TypeError, ValueError):
                pid = p[0]
            self._next = {"id": pid, "name": self.users[pid]} if pid in self.users else None
        elif s.startswith("SELECT name FROM users WHERE id="):
            try:
                pid = int(p[0])
            except (TypeError, ValueError):
                pid = p[0]
            self._next = {"name": self.users[pid]} if pid in self.users else None
        elif s.startswith("INSERT INTO phone_groups"):
            self.groups[p[0]] = p[8]
            self.rowcount = 1
        elif s.startswith("INSERT INTO phone_group_members"):
            self.members[p[0]] = (p[1], p[2])
            self.rowcount = 1
        elif s.startswith("DELETE FROM phone_group_members WHERE group_id="):
            self.rowcount = 1 if self.members.pop(p[1], None) else 0
        elif s.startswith("DELETE FROM phone_groups WHERE id="):
            for uid in [u for u, (g, _) in self.members.items() if g == p[0]]:   # FK CASCADE
                del self.members[uid]
            self.rowcount = 1 if self.groups.pop(p[0], None) is not None or p[0] in self.groups else 1
        elif s.startswith("UPDATE volte_subscriptions SET pickup_group=") or s.startswith("UPDATE ptt_subscriptions SET pickup_group="):
            self.pickup[p[1]] = p[0]
            self.rowcount = 1
        elif s.startswith("UPDATE phone_groups SET"):
            self.rowcount = 1
        # ── roles ──
        elif s.startswith("SELECT " + ", ".join(ROLE_COLS) + " FROM roles WHERE id=") or \
                s.startswith("SELECT " + ", ".join(ROLE_COLS) + ", created_at FROM roles WHERE id="):
            r = self.roles.get(p[0])
            self._next = dict(r) if r else None
        elif s.startswith("SELECT 1 FROM roles WHERE id="):
            self._next = {"1": 1} if p[0] in self.roles else None
        elif s.startswith("SELECT role_id FROM role_assignments WHERE principal_type='user' AND principal_id="):
            rid = self.assignments.get(str(p[0]))
            self._next = {"role_id": rid} if rid else None
        elif s.startswith("SELECT role_id FROM role_assignments WHERE principal_type=%s AND principal_id="):
            rid = self.assignments.get(str(p[1]))
            self._next = {"role_id": rid} if rid else None
        elif s.startswith("SELECT COUNT(*) AS n FROM role_assignments WHERE role_id="):
            self._next = {"n": sum(1 for r in self.assignments.values() if r == p[0])}
        elif s.startswith("SELECT principal_id FROM role_assignments WHERE role_id="):
            self._rows = [{"principal_id": pid} for pid, r in sorted(self.assignments.items()) if r == p[0]]
        elif s.startswith("SELECT phone_group_id FROM role_monitor_targets WHERE role_id="):
            self._rows = [{"phone_group_id": g} for r, g in sorted(self.monitor_targets) if r == p[0]]
        elif s.startswith("SELECT ptt_group_id FROM role_ptt_targets WHERE role_id="):
            self._rows = [{"ptt_group_id": g} for r, g in sorted(self.ptt_targets) if r == p[0]]
        elif s.startswith("SELECT g.mcptt_group_id FROM role_ptt_targets t JOIN ptt_groups g"):
            self._rows = [{"mcptt_group_id": "g002"} for r, g in sorted(self.ptt_targets) if r == p[0] and g == 24]
        elif s.startswith("SELECT phone_group_id FROM role_monitor_targets WHERE role_id=%s ORDER BY"):
            self._rows = [{"phone_group_id": g} for r, g in sorted(self.monitor_targets) if r == p[0]]
        elif s.startswith("INSERT INTO roles"):
            self.roles[p[0]] = _role(p[0], name=p[1], directory_write=p[2], directory_read=p[3], ptt_group_manage=p[4],
                                     monitor_call=p[5], ptt_listen=p[6], listen_visibility=p[7], history_read=p[8], org_id=p[9])
            self.rowcount = 1
        elif s.startswith("UPDATE roles SET"):
            r = self.roles[p[-1]]
            r.update(name=p[0], directory_write=p[1], directory_read=p[2], ptt_group_manage=p[3], monitor_call=p[4],
                     ptt_listen=p[5], listen_visibility=p[6], history_read=p[7], org_id=p[8])
            self.rowcount = 1
        elif s.startswith("DELETE FROM roles WHERE id="):
            self.rowcount = 1 if self.roles.pop(p[0], None) else 0
        elif s.startswith("INSERT INTO role_assignments"):
            self.assignments[str(p[1])] = p[2]
            self.rowcount = 1
        elif s.startswith("DELETE FROM role_assignments WHERE role_id="):
            self.rowcount = 1 if self.assignments.get(str(p[2])) == p[0] and self.assignments.pop(str(p[2]), None) else 0
        elif s.startswith("DELETE FROM role_monitor_targets WHERE role_id="):
            self.monitor_targets = {(r, g) for r, g in self.monitor_targets if r != p[0]}
        elif s.startswith("INSERT IGNORE INTO role_monitor_targets"):
            self.monitor_targets.add((p[0], p[1]))
        elif s.startswith("SELECT id FROM ptt_groups WHERE mcptt_group_id="):
            self._next = {"id": 24} if p[0] == "g002" else None
        elif s.startswith("DELETE FROM role_ptt_targets WHERE role_id="):
            self.ptt_targets = {(r, g) for r, g in self.ptt_targets if r != p[0]}
        elif s.startswith("INSERT IGNORE INTO role_ptt_targets"):
            self.ptt_targets.add((p[0], p[1]))
        # ── 청취 자격 동기 ──
        elif s.startswith("SELECT id FROM ptt_subscriptions WHERE user_id="):
            try:
                person = int(p[0])                       # MySQL 은 '1' 과 1 을 같게 비교한다
            except (TypeError, ValueError):
                person = p[0]
            self._rows = [{"id": i} for i in self._line_ids("ptt_subscriptions", person)]
        elif s.startswith("SELECT allow_ambient_listening FROM ptt_user_profile WHERE ptt_id="):
            self._next = {"allow_ambient_listening": self.profiles[p[0]]} if p[0] in self.profiles else None
        elif s.startswith("INSERT INTO ptt_user_profile"):
            self.profiles[p[0]] = p[1]
            self.rowcount = 1
        elif s.startswith("INSERT") or s.startswith("UPDATE"):
            self.rowcount = 1
        elif "users" in low:
            raise AssertionError(f"허용되지 않은 users 조회: {s}")
        else:
            raise AssertionError(f"unexpected SQL: {s}")
        if self._next is not None and not self._rows:
            self._rows = [self._next]                    # 실제 커서처럼 단건 결과도 fetchall 로 읽힌다(services/authz)

    def fetchone(self):
        r, self._next = self._next, None
        return r

    def fetchall(self):
        r, self._rows = self._rows, []
        return r


class _Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = _load_dispatch()
        cls.az = sys.modules["services.authz"]
        cls.notified = []
        cls.d.notify_csp = lambda *a, **k: cls.notified.append(a)

    def setUp(self):
        self.d._HAS_TABLES = True
        self.az.reset_probe()
        self.notified.clear()

    def tearDown(self):
        self.d._HAS_TABLES = None
        self.az.reset_probe()

    def _no_role_sql(self, cur: FakeCursor):
        """가입자 쪽 역할 컬럼(users.role) 을 읽는 SQL 은 어떤 경로에서도 나가면 안 된다 — 역할은 roles/role_assignments 다."""
        for s, _ in cur.executed:
            low = s.lower()
            self.assertNotIn(".role ", low, s)
            self.assertNotIn("u.role", low, s)
            self.assertNotIn("join users", low, s)

    def _notify(self, kind):
        return [a[1:] for a in self.notified if a[0] == kind]


class PhoneGroupMembershipTest(_Base):
    """전화 그룹 멤버십 — 권한이 아니다(역할 게이트 없음). pickup_group 파생 = person 단위."""

    # 관제사 = person 1: VoLTE +821310001001 (멤버 행) + PTT +82510001001 (멤버 아님 — 대표번호 포크 대상이 되면 안 된다)
    _DISPATCHER = {"+821310001001": ("volte_subscriptions", 1), "+82510001001": ("ptt_subscriptions", 1),
                   "+821310001002": ("volte_subscriptions", 2), "+82510001002": ("ptt_subscriptions", 2)}

    def test_add_member_no_role_gate_201(self):
        cur = FakeCursor({"+82310001001"})
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+82310001001", "alert_order": 0})
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(r.body["user_id"], "+82310001001")
        self._no_role_sql(cur)
        self.assertTrue(any(s.startswith("INSERT INTO phone_group_members") for s, _ in cur.executed))
        self.assertTrue(any(s.startswith("UPDATE volte_subscriptions SET pickup_group=") for s, _ in cur.executed))
        self.assertEqual(self._notify("PHONE_GROUP_CHANGED"), [("pg-t", "PUT")])
        self.assertFalse(any(s.startswith("SELECT") and "roles" in s for s, _ in cur.executed))   # 편입에 역할 조회 없음

    def test_add_member_unknown_subscriber_404(self):
        cur = FakeCursor(set())
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+82310009999"})
        self.assertEqual(r.status, 404)

    def test_add_member_unknown_group_404(self):
        cur = FakeCursor({"+82310001001"})
        self.assertEqual(self.d.pg_add_member(cur, "pg-nope", {"user_id": "+82310001001"}).status, 404)

    def test_create_group_inline_members_201(self):
        cur = FakeCursor({"+82310001001", "+82310001002"}, groups={})
        body = {"id": "pg-t2", "name": "관제", "members": [{"user_id": "+82310001001", "alert_order": 0},
                                                         {"user_id": "+82310001002", "alert_order": 1}]}
        r = self.d.pg_create(cur, body)
        self.assertEqual(r.status, 201, r.body)
        self._no_role_sql(cur)
        self.assertEqual(sum(1 for s, _ in cur.executed if s.startswith("INSERT INTO phone_group_members")), 2)
        self.assertEqual(self._notify("PHONE_GROUP_CHANGED"), [("pg-t2", "POST")])
        self.assertEqual(len(self._notify("USER_CHANGED")), 2)

    def test_create_group_rejects_scope_fields_silently_and_bad_id(self):
        cur = FakeCursor(set(), groups={})
        self.assertEqual(self.d.pg_create(cur, {"id": "dg-new", "name": "x"}).status, 400)     # 신규는 pg- 만
        r = self.d.pg_create(cur, {"name": "x", "pilot_id": "7000"})
        self.assertEqual(r.status, 400)                                                         # pilot 은 service_ref 필수
        r = self.d.pg_create(cur, {"name": "x", "monitor_scope": "all"})                        # 범위 필드는 전화 그룹에 없다
        self.assertEqual(r.status, 201, r.body)
        self.assertTrue(r.body["id"].startswith("pg-"))

    def test_add_member_derives_ptt_line_of_same_person(self):
        cur = FakeCursor(self._DISPATCHER)
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001001", "alert_order": 0})
        self.assertEqual(r.status, 201, r.body)
        self._no_role_sql(cur)
        self.assertEqual(set(cur.members), {"+821310001001"})                     # 멤버 행은 VoLTE 회선만
        self.assertEqual(cur.pickup["+821310001001"], "pg-t")
        self.assertEqual(cur.pickup["+82510001001"], "pg-t")                       # PTT 회선이 파생으로 물려받음
        self.assertIsNone(cur.pickup["+821310001002"])                             # 다른 person 은 무관
        self.assertIsNone(cur.pickup["+82510001002"])
        self.assertEqual([u for u, _a in self._notify("USER_CHANGED")], ["tel:+821310001001", "tel:+82510001001"])
        self.assertEqual(self.d.effective_phone_group(cur, "+82510001001"), "pg-t")
        self.assertIsNone(self.d.phone_group_of_user(cur, "+82510001001"))       # 멤버십 자체는 없음
        self.assertEqual(self.d.phone_group_of_person(cur, 1), "pg-t")

    def test_remove_member_clears_derived_ptt_line(self):
        cur = FakeCursor(self._DISPATCHER)
        self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001001"})
        self.notified.clear()
        r = self.d.pg_remove_member(cur, "pg-t", "+821310001001")
        self.assertEqual(r.status, 200, r.body)
        self.assertIsNone(cur.pickup["+821310001001"])
        self.assertIsNone(cur.pickup["+82510001001"])
        self.assertEqual([u for u, _a in self._notify("USER_CHANGED")], ["tel:+821310001001", "tel:+82510001001"])
        self.assertIsNone(self.d.effective_phone_group(cur, "+82510001001"))

    def test_move_member_notifies_previous_group(self):
        cur = FakeCursor(self._DISPATCHER, groups={"pg-a": None, "pg-b": None})
        self.d.pg_add_member(cur, "pg-a", {"user_id": "+821310001001"})
        self.notified.clear()
        r = self.d.pg_add_member(cur, "pg-b", {"user_id": "+821310001001"})
        self.assertEqual((r.status, r.body["moved_from"]), (201, "pg-a"))
        self.assertEqual(self._notify("PHONE_GROUP_CHANGED"), [("pg-b", "PUT"), ("pg-a", "PUT")])
        self.assertEqual(cur.pickup["+82510001001"], "pg-b")

    def test_delete_group_clears_derived_lines_of_all_members(self):
        cur = FakeCursor(self._DISPATCHER)
        self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001001", "alert_order": 0})
        self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001002", "alert_order": 1})
        self.assertEqual(cur.pickup["+82510001002"], "pg-t")
        self.notified.clear()
        r = self.d.pg_delete(cur, "pg-t")
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(cur.members, {})
        self.assertTrue(all(v is None for v in cur.pickup.values()), cur.pickup)
        self.assertEqual(self._notify("PHONE_GROUP_CHANGED"), [("pg-t", "DELETE")])
        self.assertEqual({u for u, _a in self._notify("USER_CHANGED")},
                         {"tel:+821310001001", "tel:+82510001001", "tel:+821310001002", "tel:+82510001002"})

    def test_ptt_only_member_keeps_own_membership(self):
        # S3-SCN-PTT-LISTEN 픽스처처럼 PTT 회선 자체를 멤버로 넣는 경우 — 자기 멤버십이 파생보다 우선
        cur = FakeCursor({"+82510009001": ("ptt_subscriptions", 9)})
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+82510009001"})
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(cur.pickup["+82510009001"], "pg-t")
        self.assertEqual(self.d.phone_group_of_user(cur, "+82510009001"), "pg-t")
        self.assertEqual(self.d.effective_phone_group(cur, "+82510009001"), "pg-t")

    def test_sync_is_idempotent_no_rewrite_when_unchanged(self):
        cur = FakeCursor(self._DISPATCHER)
        self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001001"})
        n_upd = sum(1 for s, _ in cur.executed if s.startswith("UPDATE"))
        self.assertEqual(self.d._sync_pickup_group(cur, "+821310001001"), [])     # 값 동일 → 쓰기 없음
        self.assertEqual(sum(1 for s, _ in cur.executed if s.startswith("UPDATE")), n_upd)


class PhoneGroupScopeTest(_Base):
    """directory_write=own 범위 — 그룹 org(생성·수정·삭제·멤버)와 멤버 person 의 org 둘 다 범위 안이어야 한다(403 out_of_scope)."""

    _LINES = {"+821310001001": ("volte_subscriptions", 1, "TEAM01"), "+821310002001": ("volte_subscriptions", 2, "TEAM02")}

    def test_create_outside_scope_403(self):
        cur = FakeCursor(self._LINES, groups={})
        r = self.d.pg_create(cur, {"name": "x", "org_id": 3}, org_codes={"TEAM01"})
        self.assertEqual((r.status, r.body["error"], r.body["org"]), (403, "out_of_scope", "TEAM02"))
        r = self.d.pg_create(cur, {"name": "x"}, org_codes={"TEAM01"})              # 조직 없는 그룹은 own 범위 밖
        self.assertEqual(r.status, 403)
        r = self.d.pg_create(cur, {"name": "x", "org_id": 2}, org_codes={"TEAM01"})
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(self.d.pg_create(cur, {"name": "y", "org_id": 99}, org_codes=None).status, 400)   # unknown_org

    def test_member_person_must_be_in_scope(self):
        cur = FakeCursor(self._LINES, groups={"pg-t": 2})
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310002001"}, org_codes={"TEAM01"})
        self.assertEqual((r.status, r.body["error"]), (403, "out_of_scope"))
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310001001"}, org_codes={"TEAM01"})
        self.assertEqual(r.status, 201, r.body)
        # 전 조직(None) 은 무제한
        r = self.d.pg_add_member(cur, "pg-t", {"user_id": "+821310002001"}, org_codes=None)
        self.assertEqual(r.status, 201, r.body)

    def test_update_delete_members_gate_on_group_org(self):
        cur = FakeCursor(self._LINES, groups={"pg-t": 3})
        self.assertEqual(self.d.pg_update(cur, "pg-t", {"name": "n"}, org_codes={"TEAM01"}).status, 403)
        self.assertEqual(self.d.pg_delete(cur, "pg-t", org_codes={"TEAM01"}).status, 403)
        self.assertEqual(self.d.pg_members(cur, "pg-t", org_codes={"TEAM01"}).status, 403)
        self.assertEqual(self.d.pg_remove_member(cur, "pg-t", "+821310002001", org_codes={"TEAM01"}).status, 403)
        # 범위 안으로 옮기는 것은 되고, 범위 밖으로 옮기는 것은 안 된다
        cur = FakeCursor(self._LINES, groups={"pg-t": 2})
        self.assertEqual(self.d.pg_update(cur, "pg-t", {"org_id": 3}, org_codes={"TEAM01"}).status, 403)
        self.assertEqual(self.d.pg_update(cur, "pg-t", {"name": "n"}, org_codes={"TEAM01"}).status, 200)
        self.assertEqual(self.d.dispatch_phone_group(cur, "GET", ("pg-t",), None, {"TEAM02"}).status, 403)
        self.assertEqual(self.d.dispatch_phone_group(cur, "GET", ("pg-t",), None, {"TEAM01"}).status, 200)


class RoleApiTest(_Base):
    """역할 CRUD·대상·배정 — mcptt_authorization.md §2.4 불변 규칙과 admin_api.md §6.8 오류 토큰."""

    _PEOPLE = {"+821310001001": ("volte_subscriptions", 1), "+82510001001": ("ptt_subscriptions", 1),
               "+82510001009": ("ptt_subscriptions", 1), "+82510002001": ("ptt_subscriptions", 2)}
    _ROLES = {"admin": {"builtin": 1, "authz_manage": 1, "audit_read": 1, "directory_write": "all", "directory_read": "all",
                        "ptt_group_manage": "all", "history_read": "all", "alarm_ack": 1, "mcptt_control": 1},
              "role-lsn": {"ptt_listen": "listed", "history_read": "scope"},
              "role-adm": {"directory_write": "own", "org_id": 2}}

    def _cur(self):
        return FakeCursor(self._PEOPLE, roles=self._ROLES)

    def test_create_not_delegable_and_preset(self):
        cur = self._cur()
        for f in ("authz_manage", "audit_read", "alarm_ack", "mcptt_control"):
            r = self.d.role_create(cur, {"name": "x", f: True})
            self.assertEqual((r.status, r.body["error"], r.body["field"]), (400, "not_delegable", f), f)
        self.assertFalse(any(s.startswith("INSERT INTO roles") for s, _ in cur.executed))
        r = self.d.role_create(cur, {"name": "전체", "preset": "full", "org_id": 2, "ptt_listen": "all"})
        self.assertEqual(r.status, 201, r.body)
        row = cur.roles[r.body["id"]]
        self.assertTrue(r.body["id"].startswith("role-"))
        self.assertEqual((row["directory_write"], row["ptt_group_manage"], row["monitor_call"], row["ptt_listen"], row["history_read"]),
                         ("own", "scope", "own", "all", "scope"))                                # 본문이 프리셋에 우선
        self.assertEqual(self._notify("ROLE_CHANGED"), [(r.body["id"], "POST")])
        self.assertEqual(self.d.role_create(cur, {"id": "manager", "name": "x"}).status, 400)   # 내장 id 불가
        self.assertEqual(self.d.role_create(cur, {"id": "role-lsn", "name": "x"}).status, 409)  # role_exists
        self.assertEqual(self.d.role_create(cur, {"name": "x", "preset": "bogus"}).status, 400)
        self.assertEqual(self.d.role_create(cur, {"name": "x", "monitor_call": "everyone"}).status, 400)

    def test_builtin_read_only(self):
        cur = self._cur()
        self.assertEqual(self.d.role_update(cur, "admin", {"name": "x"}).body["error"], "builtin")
        self.assertEqual(self.d.role_delete(cur, "admin").body["error"], "builtin")
        self.assertEqual(self.d.role_put_monitor_targets(cur, "admin", {"phone_group_ids": []}).status, 403)
        self.assertEqual(self.d.role_put_ptt_targets(cur, "admin", {"ptt_group_ids": []}).status, 403)
        self.assertEqual(self.d.role_get(cur, "admin").status, 200)
        self.assertEqual(self.d.role_get(cur, "role-nope").status, 404)

    def test_update_not_delegable_and_delete_assigned(self):
        cur = self._cur()
        self.assertEqual(self.d.role_update(cur, "role-adm", {"authz_manage": 1}).body["error"], "not_delegable")
        self.assertEqual(self.d.role_update(cur, "role-adm", {"authz_manage": False, "name": "관리"}).status, 200)   # false 는 무해
        self.assertEqual(cur.roles["role-adm"]["name"], "관리")
        self.assertEqual(self.d.role_assign(cur, "role-adm", {"principal_type": "user", "principal_id": 1}).status, 200)
        r = self.d.role_delete(cur, "role-adm")
        self.assertEqual((r.status, r.body["error"]), (409, "assigned"))
        self.d.role_unassign(cur, "role-adm", "user", "1")
        self.assertEqual(self.d.role_delete(cur, "role-adm").status, 200)
        self.assertNotIn("role-adm", cur.roles)
        self.assertEqual(self._notify("ROLE_CHANGED")[-1], ("role-adm", "DELETE"))

    def test_assign_syncs_ambient_listening_and_moves(self):
        cur = self._cur()
        r = self.d.role_assign(cur, "role-lsn", {"principal_type": "user", "principal_id": "1"})
        self.assertEqual(r.status, 200, r.body)
        self.assertIsNone(r.body["moved_from"])
        self.assertEqual(r.body["ambient_synced"], ["+82510001001", "+82510001009"])   # person 1 의 PTT 전 회선
        self.assertEqual(cur.profiles, {"+82510001001": 1, "+82510001009": 1})
        self.assertEqual(self._notify("ROLE_CHANGED"), [("1", "PUT")])
        self.assertEqual({u for u, _a in self._notify("USER_CHANGED")}, {"tel:+82510001001", "tel:+82510001009"})
        self.assertEqual(self.az.user_role_id(cur, 1), "role-lsn")
        # 청취 없는 역할로 이동 → 자격 0, moved_from
        self.notified.clear()
        r = self.d.role_assign(cur, "role-adm", {"principal_type": "user", "principal_id": "1"})
        self.assertEqual((r.status, r.body["moved_from"]), (200, "role-lsn"))
        self.assertEqual(cur.profiles, {"+82510001001": 0, "+82510001009": 0})
        self.assertEqual(cur.assignments, {"1": "role-adm"})
        # 같은 역할 재배정은 moved_from 없음·프로파일 무변화
        r = self.d.role_assign(cur, "role-adm", {"principal_type": "user", "principal_id": "1"})
        self.assertEqual((r.body["moved_from"], r.body["ambient_synced"]), (None, []))
        # PTT 회선 없는 person 2 → 프로파일 행을 만들지 않는다(자격 없음 기본)
        r = self.d.role_assign(cur, "role-adm", {"principal_type": "user", "principal_id": "2"})
        self.assertEqual(r.body["ambient_synced"], [])

    def test_unassign_clears_ambient(self):
        cur = self._cur()
        self.d.role_assign(cur, "role-lsn", {"principal_type": "user", "principal_id": "1"})
        self.notified.clear()
        r = self.d.role_unassign(cur, "role-lsn", "user", "1")
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(cur.profiles, {"+82510001001": 0, "+82510001009": 0})
        self.assertEqual(cur.assignments, {})
        self.assertEqual(self._notify("ROLE_CHANGED"), [("1", "DELETE")])
        self.assertEqual(self.d.role_unassign(cur, "role-lsn", "user", "1").status, 404)
        self.assertEqual(self.d.role_unassign(cur, "role-lsn", "console", "admin").status, 400)

    def test_assign_validation(self):
        cur = self._cur()
        r = self.d.role_assign(cur, "role-lsn", {"principal_type": "console", "principal_id": "admin"})
        self.assertEqual((r.status, r.body["error"]), (400, "invalid_principal_type"))
        self.assertEqual(self.d.role_assign(cur, "role-lsn", {"principal_id": "999"}).status, 404)      # 없는 사람
        self.assertEqual(self.d.role_assign(cur, "role-nope", {"principal_id": "1"}).status, 404)
        self.assertEqual(self.d.role_assign(cur, "role-lsn", {}).status, 400)

    def test_role_update_ptt_listen_toggle_resyncs_assignees(self):
        cur = self._cur()
        self.d.role_assign(cur, "role-adm", {"principal_type": "user", "principal_id": "1"})
        self.assertEqual(cur.profiles, {})
        r = self.d.role_update(cur, "role-adm", {"ptt_listen": "all"})
        self.assertEqual((r.status, r.body["ambient_synced"]), (200, ["+82510001001", "+82510001009"]))
        self.assertEqual(cur.profiles, {"+82510001001": 1, "+82510001009": 1})
        r = self.d.role_update(cur, "role-adm", {"ptt_listen": "none"})
        self.assertEqual(cur.profiles, {"+82510001001": 0, "+82510001009": 0})
        r = self.d.role_update(cur, "role-adm", {"name": "n"})                               # 범위 변화 없음 → 동기 없음
        self.assertEqual(r.body["ambient_synced"], [])

    def test_targets(self):
        cur = self._cur()
        cur.groups = {"pg-a": None, "pg-b": None}
        r = self.d.role_put_monitor_targets(cur, "role-lsn", {"phone_group_ids": ["pg-a", "pg-b", "pg-a"]})
        self.assertEqual((r.status, r.body["phone_group_ids"]), (200, ["pg-a", "pg-b"]))
        self.assertEqual(self.d.role_put_monitor_targets(cur, "role-lsn", {"phone_group_ids": ["pg-nope"]}).status, 400)
        self.assertEqual(self.d.role_put_monitor_targets(cur, "role-lsn", {}).status, 400)
        r = self.d.role_put_ptt_targets(cur, "role-lsn", {"ptt_group_ids": ["g002"]})
        self.assertEqual((r.status, r.body["ptt_group_ids"]), (200, ["g002"]))
        self.assertEqual(cur.ptt_targets, {("role-lsn", 24)})
        self.assertEqual(self.d.role_put_ptt_targets(cur, "role-lsn", {"ptt_group_ids": ["g999"]}).status, 400)
        self.assertEqual(self.az.role_targets(cur, "role-lsn"), ({"pg-a", "pg-b"}, {24}))
        self.assertEqual(self._notify("ROLE_CHANGED")[-1], ("role-lsn", "PUT"))


class SourceTest(_Base):
    """소스 정적 확인 — 관제 그룹(dispatch_groups) 잔재·가입자 역할 게이트 없음, 통지 이름은 PHONE_GROUP_CHANGED/ROLE_CHANGED,
    라우트는 /api/v1/phone-groups·/api/v1/roles 둘(구 /api/v1/dispatch-groups 없음)."""

    def test_source(self):
        with open(os.path.join(_CSC_SRC, "handlers", "dispatch.py"), encoding="utf-8") as f:
            src = f.read()
        self.assertNotIn("_member_role_ok", src)
        self.assertNotIn("u.role", src)
        self.assertNotIn("dispatch_groups", src)
        self.assertNotIn("DISPATCH_GROUP_CHANGED", src)
        self.assertIn('notify_csp("PHONE_GROUP_CHANGED"', src)
        self.assertIn('notify_csp("ROLE_CHANGED"', src)
        self.assertEqual([p for p, _h, _k in self.d.CIMS_DISPATCH_HANDLER_LIST], ["/api/v1/phone-groups", "/api/v1/roles"])
        self.assertEqual({p for p, _h, _k in self.d.CIMS_DISPATCH_HANDLER_LIST},
                         {re.sub(r"/\{.*$", "", d["path"]).rstrip("/").split("/{")[0][:len("/api/v1/phone-groups")]
                          if d["path"].startswith("/api/v1/phone-groups") else "/api/v1/roles" for d in self.d.CIMS_DISPATCH_API_DOCS})


if __name__ == "__main__":
    unittest.main()
