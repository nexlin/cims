"""csc/src/services/mcptt.py — GMS XCAP 그룹 CRUD(가입자 주체) 단위 시험 (오프라인, DB 없음).

mcptt_authorization.md §3 / TS 24.481 Ut PUT·DELETE: 생성 = 프로파일 allow_create_group, 수정·삭제 = 소유
(authorized_user_id == 토큰 가입자 users.id). 본문 = get_group_xml 이 내는 문서와 같은 포맷.
DB 쓰기(gms_write_group)는 가짜 커넥션으로 SQL 조립을 검증한다.

  python3 -m unittest tests.test_csc_gms_group_crud
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

import services.mcptt as m  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

PTT_DOM = "ptt.mnc033.mcc450.3gppnetwork.org"
OWNER_LOGIN, OWNER_UID, OWNER_PTT = "disp01", 5020, "+82510001001"
OTHER_LOGIN, OTHER_UID, OTHER_PTT = "disp02", 5021, "+82510001002"


def _args(method, path, body=None, headers=None):
    h = {"authorization": "Bearer x"}
    h.update(headers or {})
    return HandlerArgs(method, path, "127.0.0.1", 0, headers=h, body=body)


def _run(coro):
    return asyncio.run(coro)


def _doc(uri, name, members, session_type="prearranged", extra=""):
    entries = "".join(
        f'<entry uri="{u}"><rl:display-name>{u}</rl:display-name>'
        f'<mcpttgi:participant-type>{r}</mcpttgi:participant-type>'
        f'<mcpttgi:user-priority>{p}</mcpttgi:user-priority></entry>' for u, r, p in members)
    return (f'<?xml version="1.0" encoding="UTF-8"?><group xmlns="urn:oma:xml:poc:list-service" '
            f'xmlns:rl="urn:ietf:params:xml:ns:resource-lists" xmlns:cp="urn:ietf:params:xml:ns:common-policy" '
            f'xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"><list-service uri="{uri}">'
            f'<display-name>{name}</display-name><list>{entries}</list>'
            f'<mcpttgi:session-type>{session_type}</mcpttgi:session-type>{extra}</list-service></group>')


class _Base(unittest.TestCase):
    def setUp(self):
        self._saved = (dict(m.GROUPS), dict(m.LOGIN_ACCOUNTS), dict(m.PTT_PROFILES), m._DB_CONFIG,
                       m.extract_token, m.notify_csp, m.save_group_to_file, m.delete_group_file)
        m.GROUPS.clear(); m.LOGIN_ACCOUNTS.clear(); m.PTT_PROFILES.clear()
        m._DB_CONFIG = None                      # 파일 폴백 경로 — DB 없이 인가·파싱만 검증
        m.notify_csp = lambda *a, **k: None
        m.save_group_to_file = lambda *a, **k: None
        m.delete_group_file = lambda *a, **k: None
        m.LOGIN_ACCOUNTS[OWNER_LOGIN] = {"user_id": OWNER_UID, "mcptt_id": f"tel:{OWNER_PTT}", "password": "", "name": "관제1석"}
        m.LOGIN_ACCOUNTS[OTHER_LOGIN] = {"user_id": OTHER_UID, "mcptt_id": f"tel:{OTHER_PTT}", "password": "", "name": "관제2석"}
        self.token = {"sub": OWNER_LOGIN, "mcptt_id": f"tel:{OWNER_PTT}"}
        m.extract_token = lambda hdr: self.token if hdr else None

    def tearDown(self):
        g, la, pp, db, et, nc, sg, dg = self._saved
        m.GROUPS.clear(); m.GROUPS.update(g)
        m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(la)
        m.PTT_PROFILES.clear(); m.PTT_PROFILES.update(pp)
        m._DB_CONFIG, m.extract_token, m.notify_csp, m.save_group_to_file, m.delete_group_file = db, et, nc, sg, dg

    def _grant_create(self, ptt=OWNER_PTT):
        m.PTT_PROFILES[ptt] = dict(m.DEFAULT_USER_PROFILE, allow_create_group=True)

    def _existing(self, gid, owner_uid, members=()):
        m.GROUPS[f"tel:{gid}"] = {
            "display_name": gid, "etag": f"etag_{gid}", "authorized_user_id": owner_uid,
            "authorized_user": "", "members": [{"uri": f"tel:{u}", "name": u, "role": "participant", "priority": 0}
                                                for u in members],
        }

    def _put(self, gid, xml, xui=OWNER_PTT, headers=None):
        return _run(m.handle_group_management(
            _args("PUT", f"/org.openmobilealliance.groups/users/tel:{xui}/tel:{gid}",
                  body=xml.encode(), headers=headers), {}))

    def _delete(self, gid, xui=OWNER_PTT):
        return _run(m.handle_group_management(
            _args("DELETE", f"/org.openmobilealliance.groups/users/tel:{xui}/tel:{gid}"), {}))


class ParseTests(unittest.TestCase):
    def test_roundtrip_with_get_group_xml(self):
        m.GROUPS["tel:g-0000abcd"] = {
            "display_name": "관제채널", "etag": "e", "video_enabled": True, "priority": 3, "encryption": False,
            "emergency_call": True, "emergency_alert": False, "allow_sds": True, "allow_fd": True,
            "max_sds_size": 2000, "max_auto_recv": 4096, "org_code": "TEAM01", "group_type": "chat",
            "max_members": 7, "require_affiliation": False, "authorized_user": "tel:+82510001001",
            "members": [{"uri": "tel:+82510001001", "name": "관제1석", "role": "chair", "priority": 1, "title": "팀장"},
                        {"uri": "tel:+82500000001", "name": "테스트001", "role": "participant", "priority": 5}],
        }
        try:
            xml, _ = m.get_group_xml("tel:g-0000abcd")
            d = m.parse_group_document_xml(xml)
        finally:
            m.GROUPS.pop("tel:g-0000abcd", None)
        self.assertEqual(d["display_name"], "관제채널")
        self.assertEqual(d["group_type"], "chat")
        self.assertEqual((d["video_enabled"], d["priority"], d["encryption"]), (True, 3, False))
        self.assertEqual((d["emergency_call"], d["emergency_alert"]), (True, False))
        self.assertEqual((d["allow_sds"], d["allow_fd"], d["max_sds_size"], d["max_auto_recv"]), (True, True, 2000, 4096))
        self.assertEqual((d["max_members"], d["require_affiliation"], d["org_code"]), (7, False, "TEAM01"))
        self.assertEqual([(x["user_id"], x["role"], x["priority"]) for x in d["members"]],
                         [("+82510001001", "chair", 1), ("+82500000001", "participant", 5)])

    def test_missing_elements_are_none_and_members_absent(self):
        d = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service"><list-service uri="tel:g-00000001">'
            '<display-name>n</display-name></list-service></group>')
        self.assertEqual(d["display_name"], "n")
        self.assertIsNone(d["members"])
        self.assertIsNone(d["group_type"])
        self.assertIsNone(d["priority"])

    def test_rejects_malformed_dtd_oversize_bad_enums(self):
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<group><list-service>")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml('<!DOCTYPE x [<!ENTITY a "b">]><group/>')
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<a>" + "x" * (m._GMS_MAX_BODY + 1) + "</a>")
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [], session_type="party"))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml(_doc("tel:g-00000001", "n", [("tel:+82500000001", "boss", 1)]))
        with self.assertRaises(ValueError):
            m.parse_group_document_xml("<group xmlns=\"urn:oma:xml:poc:list-service\"/>")

    def test_member_uri_forms_normalize_to_msisdn(self):
        d = m.parse_group_document_xml(_doc("tel:g-00000001", "n", [
            ("tel:+82500000001", "participant", 0), (f"sip:+82500000002@{PTT_DOM}", "chair", 2), ("82500000003", "participant", 1)]))
        self.assertEqual([x["user_id"] for x in d["members"]], ["+82500000001", "+82500000002", "+82500000003"])
        self.assertEqual(d["members"][0]["mcptt_id"], "tel:+82500000001")
        self.assertIsNone(d["members"][2]["mcptt_id"])


class IdTests(unittest.TestCase):
    def test_validate_new_group_id(self):
        self.assertIsNone(m.validate_new_gms_group_id("g-0a1b2c3d"))
        self.assertIn("reserved", m.validate_new_gms_group_id("adhoc-x"))
        self.assertIn("reserved", m.validate_new_gms_group_id("priv-x"))
        self.assertIn("8 lowercase hex", m.validate_new_gms_group_id("g001"))
        self.assertIn("8 lowercase hex", m.validate_new_gms_group_id("g-0A1B2C3D"))
        self.assertIn("required", m.validate_new_gms_group_id(""))
        self.assertIn("not the PTT domain", m.validate_new_gms_group_id("g-0a1b2c3d", "example.com"))
        self.assertIsNone(m.validate_new_gms_group_id("g-0a1b2c3d", m.IDMS_DOMAIN))

    def test_gid_from_uri(self):
        self.assertEqual(m._gms_gid_from_uri("tel:g-0a1b2c3d"), ("g-0a1b2c3d", ""))
        self.assertEqual(m._gms_gid_from_uri(f"sip:g-0a1b2c3d@{PTT_DOM}"), ("g-0a1b2c3d", PTT_DOM))
        self.assertEqual(m._gms_gid_from_uri("g001"), ("g001", ""))

    def test_token_user_id_and_ptt_id(self):
        saved = dict(m.LOGIN_ACCOUNTS)
        try:
            m.LOGIN_ACCOUNTS["disp01"] = {"user_id": 5020}
            self.assertEqual(m._token_user_id({"sub": "disp01"}), 5020)
            self.assertIsNone(m._token_user_id({"sub": "nobody"}))
            self.assertIsNone(m._token_user_id({}))
        finally:
            m.LOGIN_ACCOUNTS.clear(); m.LOGIN_ACCOUNTS.update(saved)
        self.assertEqual(m._requester_ptt_id({"mcptt_id": "tel:+82510001001"}), "+82510001001")
        self.assertEqual(m._requester_ptt_id({"mcptt_id": f"sip:+82510001001@{PTT_DOM}"}), "+82510001001")


class GateTests(_Base):
    def test_put_new_requires_allow_create_group(self):
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []))
        self.assertEqual(r.status, 403)
        self.assertIn("group_creation_not_allowed", r.body)

    def test_put_new_rejects_bad_id_even_with_grant(self):
        self._grant_create()
        self.assertEqual(self._put("g001", _doc("tel:g001", "n", [])).status, 400)
        r = self._put("adhoc-1", _doc("tel:adhoc-1", "n", []))
        self.assertEqual(r.status, 400)
        self.assertIn("reserved_prefix", r.body)

    def test_put_new_creates_with_owner_201(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "관제채널", [("tel:+82500000001", "participant", 0)]))
        self.assertEqual(r.status, 201, r.body)
        self.assertIn("Etag", r.headers)
        g = m.GROUPS["tel:g-0a1b2c3d"]
        self.assertEqual(g["display_name"], "관제채널")
        self.assertEqual(g["authorized_user_id"], OWNER_UID)
        self.assertEqual([x["uri"] for x in g["members"]], ["tel:+82500000001"])

    def test_put_existing_owner_updates_200(self):
        self._existing("g-0a1b2c3d", OWNER_UID, members=["+82500000001"])
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "새이름", [("tel:+82500000002", "chair", 1)]))
        self.assertEqual(r.status, 200, r.body)
        g = m.GROUPS["tel:g-0a1b2c3d"]
        self.assertEqual(g["display_name"], "새이름")
        self.assertEqual([x["uri"] for x in g["members"]], ["tel:+82500000002"])

    def test_put_existing_other_owner_409_ownerless_403(self):
        self._existing("g-0a1b2c3d", OTHER_UID)
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []))
        self.assertEqual(r.status, 409)
        self.assertIn("uri_taken", r.body)
        self._existing("g001", None)
        r = self._put("g001", _doc("tel:g001", "n", []))
        self.assertEqual(r.status, 403)
        self.assertIn("not_group_owner", r.body)

    def test_put_if_match_mismatch_412(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []), headers={"if-match": '"stale"'})
        self.assertEqual(r.status, 412)

    def test_put_invalid_document_400(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", "<group>")
        self.assertEqual(r.status, 400)
        self.assertIn("invalid_group_document", r.body)

    def test_delete_owner_200_other_403_missing_404(self):
        self._existing("g-0a1b2c3d", OWNER_UID)
        self.assertEqual(self._delete("g-0a1b2c3d").status, 200)
        self.assertNotIn("tel:g-0a1b2c3d", m.GROUPS)
        self._existing("g-0a1b2c3e", OTHER_UID)
        r = self._delete("g-0a1b2c3e")
        self.assertEqual(r.status, 403)
        self.assertIn("tel:g-0a1b2c3e", m.GROUPS)
        self.assertEqual(self._delete("g-ffffffff").status, 404)

    def test_tree_owner_mismatch_403_and_no_token_401(self):
        self._grant_create()
        r = self._put("g-0a1b2c3d", _doc("tel:g-0a1b2c3d", "n", []), xui=OTHER_PTT)
        self.assertEqual(r.status, 403)
        r = _run(m.handle_group_management(
            HandlerArgs("PUT", f"/org.openmobilealliance.groups/users/tel:{OWNER_PTT}/tel:g-0a1b2c3d", "127.0.0.1", 0, headers={}), {}))
        self.assertEqual(r.status, 401)

    def test_list_includes_owner_and_marks_is_owner(self):
        import json
        self._existing("g-0a1b2c3d", OWNER_UID)                       # 소유자이나 비멤버
        self._existing("g001", None, members=[OWNER_PTT])            # 멤버이나 콘솔 그룹(소유자 없음)
        self._existing("g002", OTHER_UID)                            # 무관
        r = _run(m.handle_group_management(
            _args("GET", f"/org.openmobilealliance.groups/users/tel:{OWNER_PTT}"), {}))
        self.assertEqual(r.status, 200)
        rows = {x["uri"]: x["is_owner"] for x in json.loads(r.body)}
        self.assertEqual(rows, {"tel:g-0a1b2c3d": True, "tel:g001": False})


class _FakeCursor:
    def __init__(self, known_subs, existing_pk=None):
        self.sql, self.known, self.pk = [], set(known_subs), existing_pk
        self.lastrowid, self.rowcount, self._rows = 77, 0, []

    def execute(self, q, args=None):
        self.sql.append((q, tuple(args) if args is not None else ()))
        if q.startswith("SELECT id FROM ptt_subscriptions"):
            self._rows = [{"id": a} for a in args if a in self.known]
        elif q.startswith("SELECT id FROM ptt_groups"):
            self._rows = [{"id": self.pk}] if self.pk else []
        elif q.startswith("DELETE FROM ptt_groups"):
            self.rowcount = 1 if self.pk else 0
        else:
            self._rows = []

    def fetchall(self): return list(self._rows)
    def fetchone(self): return self._rows[0] if self._rows else None
    def __enter__(self): return self
    def __exit__(self, *a): return False


class _FakeConn:
    def __init__(self, cur): self.cur, self.committed = cur, False
    def cursor(self): return self.cur
    def commit(self): self.committed = True
    def __enter__(self): return self
    def __exit__(self, *a): return False


class DbWriteTests(unittest.TestCase):
    def setUp(self):
        self._saved = (m._db_connect, m.sync_group_from_db)
        m.sync_group_from_db = lambda gid: True

    def tearDown(self):
        m._db_connect, m.sync_group_from_db = self._saved

    def test_create_inserts_defaults_owner_and_members(self):
        cur = _FakeCursor({"+82500000001", "+82500000002"})
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "관제채널",
                                              [("tel:+82500000001", "chair", 1), ("tel:+82500000002", "participant", 0)], "chat"))
        st, err = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=True)
        self.assertEqual((st, err), (0, {}))
        ins = next(q for q, a in cur.sql if q.startswith("INSERT INTO ptt_groups"))
        args = next(a for q, a in cur.sql if q.startswith("INSERT INTO ptt_groups"))
        self.assertIn("authorized_user_id", ins)
        self.assertEqual(args[0], "g-0a1b2c3d"); self.assertEqual(args[1], "관제채널"); self.assertEqual(args[-1], 5020)
        self.assertEqual(args[2 + m._GMS_ATTR_COLS.index("group_type")], "chat")
        self.assertEqual(args[2 + m._GMS_ATTR_COLS.index("priority")], 5)            # 기본값
        mem = [a for q, a in cur.sql if q.startswith("INSERT IGNORE INTO ptt_group_members")]
        self.assertEqual([(a[1], a[2], a[3]) for a in mem], [("+82500000001", 1, "chair"), ("+82500000002", 0, "participant")])
        self.assertTrue(any(q.startswith("DELETE FROM ptt_group_members") for q, _ in cur.sql))

    def test_unknown_member_400_and_no_insert(self):
        cur = _FakeCursor({"+82500000001"})
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(_doc("tel:g-0a1b2c3d", "n", [("tel:+82599999999", "participant", 0)]))
        st, err = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=True)
        self.assertEqual(st, 400); self.assertEqual(err["error"], "unknown_member"); self.assertEqual(err["detail"], ["+82599999999"])
        self.assertFalse(any(q.startswith("INSERT INTO ptt_groups") for q, _ in cur.sql))

    def test_update_only_given_fields_keeps_members_when_absent(self):
        cur = _FakeCursor(set(), existing_pk=42)
        m._db_connect = lambda: _FakeConn(cur)
        doc = m.parse_group_document_xml(
            '<group xmlns="urn:oma:xml:poc:list-service" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">'
            '<list-service uri="tel:g-0a1b2c3d"><display-name>새이름</display-name>'
            '<mcpttgi:on-network-group-priority>2</mcpttgi:on-network-group-priority></list-service></group>')
        st, _ = m.gms_write_group("g-0a1b2c3d", doc, 5020, create=False)
        self.assertEqual(st, 0)
        upd = next((q, a) for q, a in cur.sql if q.startswith("UPDATE ptt_groups"))
        self.assertEqual(upd[0], "UPDATE ptt_groups SET name=%s, priority=%s WHERE id=%s")
        self.assertEqual(upd[1], ("새이름", 2, 42))
        self.assertFalse(any(q.startswith("DELETE FROM ptt_group_members") for q, _ in cur.sql))

    def test_update_missing_group_404_and_delete(self):
        cur = _FakeCursor(set(), existing_pk=None)
        m._db_connect = lambda: _FakeConn(cur)
        st, err = m.gms_write_group("g-0a1b2c3d", {"display_name": "x", "members": None}, 5020, create=False)
        self.assertEqual((st, err["error"]), (404, "not_found"))
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d")[0], 404)
        cur2 = _FakeCursor(set(), existing_pk=42)
        m._db_connect = lambda: _FakeConn(cur2)
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d"), (0, {}))

    def test_no_db_config_503(self):
        m._db_connect = lambda: None
        self.assertEqual(m.gms_write_group("g-0a1b2c3d", {}, 1, True)[0], 503)
        self.assertEqual(m.gms_delete_group("g-0a1b2c3d")[0], 503)


if __name__ == "__main__":
    unittest.main()
