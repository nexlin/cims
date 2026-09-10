"""csc/src/services/subscriptions.py + handlers/admin.py 쓰기 게이트 단위 시험 (오프라인, DB 없음).

테이블 = 접속환경 kind(volte/voip/ptt — sip_service_model.md §2-9, db_schema.md):
  · 레지스트리 — API 세그먼트/응답 키/선택 테이블 프로브/전화 가족 UNION/번호 유일성/sip_transport ANY
  · 관리 API 쓰기 게이트 — voip 회선 service_ref 필수(400), 다른 kind 의 서비스(400 service_kind_mismatch),
    번호 유일성(409 number_exists — 3 테이블 + phone_groups.pilot_id), voip 테이블 미마이그레이션(503)
  python3 -m unittest tests.test_csc_subscriptions
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

from tests.test_csc_subscription_realm import _load_admin  # noqa: E402  (admin.py 적재 — DB·MCPTT·AuC 스텁)
from services import subscriptions as subs  # noqa: E402
from services import access_services as acc  # noqa: E402


class _Cur:
    """레지스트리·admin 게이트가 내는 SQL 만 흉내 내는 DictCursor. lines = {msisdn: (kind, user_id)}, pilots = {pilot}."""

    def __init__(self, lines=None, pilots=(), voip_table=True, phone_groups_table=True):
        self.lines = dict(lines or {})
        self.pilots = set(pilots)
        self.voip_table = voip_table
        self.pg_table = phone_groups_table
        self.executed = []
        self._rows = []

    def execute(self, sql, params=None):
        s = " ".join(sql.split())
        p = tuple(params) if params else ()
        self.executed.append((s, p))
        self._rows = []
        if s.startswith("SHOW TABLES LIKE %s"):
            present = (p[0] != "voip_subscriptions" or self.voip_table)
            self._rows = [{"Tables_in_cims": p[0]}] if present else []
        elif s.startswith("SHOW TABLES LIKE 'phone_groups'"):
            self._rows = [{"Tables_in_cims": "phone_groups"}] if self.pg_table else []
        elif s.startswith("SHOW COLUMNS FROM"):
            self._rows = [{"Field": "x"}]
        elif s.startswith("SELECT 1 FROM ") and " WHERE id=%s" in s:
            table = s.split("SELECT 1 FROM ")[1].split(" ")[0]
            kind = subs.kind_of_table(table)
            hit = p and p[0] in self.lines and self.lines[p[0]][0] == kind
            self._rows = [{"1": 1}] if hit else []
        elif s.startswith("SELECT user_id FROM ") and " WHERE id=%s" in s:
            table = s.split("SELECT user_id FROM ")[1].split(" ")[0]
            kind = subs.kind_of_table(table)
            ln = self.lines.get(p[0]) if p else None
            self._rows = [{"user_id": ln[1]}] if ln and ln[0] == kind else []
        elif s.startswith("SELECT id FROM phone_groups WHERE pilot_id=%s"):
            self._rows = [{"id": "pg-x"}] if p and p[0] in self.pilots else []
        elif s.startswith("SELECT id FROM users WHERE id=%s"):
            self._rows = [{"id": p[0]}]
        elif s.startswith("INSERT INTO "):
            table = s.split("INSERT INTO ")[1].split(" ")[0]
            self.lines[p[0]] = (subs.kind_of_table(table), p[1])
        else:
            raise AssertionError(f"unexpected SQL: {s}")

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)

    # with 문 호환
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class _Conn:
    def __init__(self, cur):
        self.cur = cur

    def cursor(self):
        return self.cur

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class RegistryTests(unittest.TestCase):
    def setUp(self):
        subs.reset_probe()

    def test_kinds_segments_keys(self):
        self.assertEqual(subs.KINDS, ("volte", "voip", "ptt"))
        self.assertEqual(subs.API_SEGMENTS, {"call": "volte", "voip": "voip", "ptt": "ptt"})
        self.assertEqual([subs.RESPONSE_KEYS[k] for k in subs.KINDS],
                         ["call_subscriptions", "voip_subscriptions", "ptt_subscriptions"])
        self.assertEqual(subs.table("mcptt"), "ptt_subscriptions")                 # 별칭
        self.assertEqual(subs.kind_of_table("voip_subscriptions"), "voip")
        self.assertEqual(subs.kind_of_table("x"), "")

    def test_kind_matches_exact_with_ptt_alias(self):
        self.assertTrue(subs.kind_matches("voip", "voip"))
        self.assertTrue(subs.kind_matches("ptt", "mcptt"))
        self.assertTrue(subs.kind_matches("volte", ""))                            # kind 없는 레코드는 이름으로 인정
        self.assertFalse(subs.kind_matches("volte", "voip"))
        self.assertFalse(subs.kind_matches("voip", "volte"))
        self.assertFalse(subs.kind_matches("ptt", "voip"))

    def test_optional_table_probe_is_cached(self):
        cur = _Cur(voip_table=False)
        self.assertEqual(subs.tables(cur), [("volte", "volte_subscriptions"), ("ptt", "ptt_subscriptions")])
        self.assertEqual(subs.phone_tables(cur), [("volte", "volte_subscriptions")])
        n = len(cur.executed)
        subs.tables(cur)
        self.assertEqual(len(cur.executed), n)                                      # 두 번째는 SQL 없음(캐시)
        subs.reset_probe()
        cur = _Cur(voip_table=True)
        self.assertEqual([k for k, _t in subs.tables(cur)], ["volte", "voip", "ptt"])
        self.assertEqual(subs.phone_union_sql(cur),
                         "(SELECT id, user_id FROM volte_subscriptions UNION ALL SELECT id, user_id FROM voip_subscriptions)")
        self.assertEqual(subs.all_union_sql(cur, "id").count("UNION ALL"), 2)

    def test_find_line_and_number_taken(self):
        cur = _Cur(lines={"+8213": ("volte", 1), "+8221": ("voip", 1), "+825": ("ptt", 2)}, pilots={"+82210001000"})
        self.assertEqual(subs.find_line(cur, "+8221"), ("voip", "voip_subscriptions", 1))
        self.assertEqual(subs.find_line(cur, "+825")[0], "ptt")
        self.assertIsNone(subs.find_line(cur, "+9"))
        self.assertEqual(subs.number_taken(cur, "+8213"), "volte_subscriptions")
        self.assertEqual(subs.number_taken(cur, "+8221"), "voip_subscriptions")
        self.assertIsNone(subs.number_taken(cur, "+8221", exclude_kind="voip"))    # 같은 테이블은 PK 가 본다
        self.assertEqual(subs.number_taken(cur, "+82210001000"), "phone_groups")   # 대표번호 주소 공간
        self.assertIsNone(subs.number_taken(cur, "+9"))

    def test_sip_transport_any(self):
        for v in (None, "", "ANY", "any", " Any "):
            self.assertIsNone(subs.parse_sip_transport(v), v)
        self.assertEqual(subs.parse_sip_transport("tls"), "TLS")
        with self.assertRaises(ValueError):
            subs.parse_sip_transport("SCTP")
        self.assertEqual(subs.wire_sip_transport(None), "ANY")
        self.assertEqual(subs.wire_sip_transport(""), "ANY")
        self.assertEqual(subs.wire_sip_transport("tcp"), "TCP")


class WriteGateTests(unittest.TestCase):
    """admin.py 게이트 — _service_kind_gate / _add_subscription 의 kind·유일성·테이블 프로브."""

    @classmethod
    def setUpClass(cls):
        cls.a = _load_admin()
        cls.tmp = tempfile.mkdtemp(prefix="csc_subs_")

    def setUp(self):
        subs.reset_probe()
        self.a._HAS_HA1_COL = True
        self.a._HAS_PICKUP_COL = False
        # AuC 스텁 — AKA 컬럼 없음(digest 만)
        self.a._auc.has_aka_columns = lambda cur: False
        self.a._dispatch.phone_group_of_person = lambda cur, pid: None

    def _cfg(self, mirror=None, services=None):
        rt = os.path.join(self.tmp, f"rt{len(os.listdir(self.tmp))}")
        cfg = {"CimsRuntimeDir": rt}
        if mirror:
            d = os.path.join(rt, "collections", "csp", "access_services")
            os.makedirs(d, exist_ok=True)
            for i, r in enumerate(mirror):
                with open(os.path.join(d, f"{i + 1}.json"), "w", encoding="utf-8") as f:
                    json.dump(dict(r, enabled=True, priority=100), f)
        if services is not None:
            cfg["Provisioning"] = {"Services": services}
        acc.configure(cfg)
        return cfg

    MIRROR = [{"name": "volte", "kind": "volte", "domain": "volte.sot"},
              {"name": "voip", "kind": "voip", "domain": "voip.sot"},
              {"name": "mcptt", "kind": "ptt", "domain": "ptt.sot"}]

    def test_gate_mirror(self):
        cfg = self._cfg(mirror=self.MIRROR)
        g = self.a._service_kind_gate
        self.assertIsNone(g(cfg, "voip", "voip"))
        self.assertIsNone(g(cfg, "mcptt", "ptt"))
        self.assertIsNone(g(cfg, "", "voip"))                                       # 빈 ref 는 여기서 안 막는다
        self.assertIsNone(g(cfg, "unknown-name", "voip"))                           # 미지 이름 = H(A1) 파생이 400
        r = g(cfg, "voip", "volte")
        self.assertEqual((r.status, r.body["error"], r.body["kind"], r.body["service_kind"]),
                         (400, "service_kind_mismatch", "volte", "voip"))
        self.assertEqual(g(cfg, "volte", "voip").body["service_kind"], "volte")
        self.assertEqual(g(cfg, "mcptt", "volte").body["service_kind"], "ptt")

    def test_gate_config_fallback(self):
        cfg = self._cfg(services={"volte": {"name": "volte", "domain": "v"}, "voip": {"name": "voip", "domain": "w"},
                                  "ptt": {"name": "mcptt", "domain": "p"}})
        g = self.a._service_kind_gate
        self.assertIsNone(g(cfg, "voip", "voip"))
        self.assertEqual(g(cfg, "voip", "volte").body["error"], "service_kind_mismatch")
        self.assertEqual(g(cfg, "mcptt", "voip").body["service_kind"], "ptt")

    def _add(self, cur, svc, body, cfg):
        self.a._get_db = lambda config: _Conn(cur)
        return asyncio.run(self.a._add_subscription("7", svc, body, cfg))

    def test_add_voip_requires_service_ref_and_kind(self):
        cfg = self._cfg(mirror=self.MIRROR)
        cur = _Cur()
        r = self._add(cur, "voip", {"id": "+82210001009", "imsi": "82210001009", "passwd": "x"}, cfg)
        self.assertEqual((r.status, r.body["error"]), (400, "service_ref required for voip"))
        r = self._add(cur, "voip", {"id": "+82210001009", "imsi": "82210001009", "passwd": "x", "service_ref": "volte"}, cfg)
        self.assertEqual((r.status, r.body["error"]), (400, "service_kind_mismatch"))
        r = self._add(cur, "call", {"id": "+821310001009", "imsi": "4500", "passwd": "x", "service_ref": "voip"}, cfg)
        self.assertEqual((r.status, r.body["error"]), (400, "service_kind_mismatch"))
        self.assertFalse(any(s.startswith("INSERT") for s, _ in cur.executed))

    def test_add_voip_ok_then_number_exists_across_tables_and_pilot(self):
        cfg = self._cfg(mirror=self.MIRROR)
        cur = _Cur(lines={"+821310001001": ("volte", 1)}, pilots={"+82210001000"})
        r = self._add(cur, "voip", {"id": "+82210001009", "imsi": "82210001009", "passwd": "x", "service_ref": "voip",
                                    "sip_transport": "ANY"}, cfg)
        self.assertEqual((r.status, r.body), (201, {"id": "+82210001009"}))
        ins = [(s, p) for s, p in cur.executed if s.startswith("INSERT INTO voip_subscriptions")]
        self.assertEqual(len(ins), 1)
        self.assertIsNone(ins[0][1][5])                                             # sip_transport ANY → NULL
        # 같은 번호를 volte 로 — 다른 테이블의 번호(409), 대표번호도 409
        r = self._add(cur, "call", {"id": "+82210001009", "imsi": "4500", "passwd": "x", "service_ref": "volte"}, cfg)
        self.assertEqual((r.status, r.body["error"], r.body["where"]), (409, "number_exists", "voip_subscriptions"))
        r = self._add(cur, "ptt", {"id": "+821310001001", "imsi": "4500", "passwd": "x", "service_ref": "mcptt"}, cfg)
        self.assertEqual((r.status, r.body["where"]), (409, "volte_subscriptions"))
        r = self._add(cur, "call", {"id": "+82210001000", "imsi": "4500", "passwd": "x", "service_ref": "volte"}, cfg)
        self.assertEqual((r.status, r.body["where"]), (409, "phone_groups"))

    def test_voip_table_missing_is_503(self):
        cfg = self._cfg(mirror=self.MIRROR)
        cur = _Cur(voip_table=False)
        r = self._add(cur, "voip", {"id": "+82210001009", "imsi": "82210001009", "passwd": "x", "service_ref": "voip"}, cfg)
        self.assertEqual((r.status, r.body["error"]), (503, "schema_not_migrated"))
        self.assertIn("migrate_voip_subscriptions.sql", r.body["detail"])
        r = asyncio.run(self.a._list_subscriptions("7", "voip", cfg))
        self.assertEqual(r.status, 503)
        r = asyncio.run(self.a._delete_subscription("7", "voip", "+82210001009", cfg))
        self.assertEqual(r.status, 503)

    def test_bad_transport_message(self):
        cfg = self._cfg(mirror=self.MIRROR)
        r = self._add(_Cur(), "call", {"id": "+8213", "imsi": "1", "passwd": "x", "service_ref": "volte",
                                       "sip_transport": "SCTP"}, cfg)
        self.assertEqual((r.status, r.body["error"]), (400, "sip_transport must be one of UDP/TCP/TLS/ANY"))


if __name__ == "__main__":
    unittest.main()
