"""AuC 서비스 단위시험 — 가짜 DB 커넥션으로 AV 발급/재동기/오류 사상을 본다 (sip_access_security.md §8.2)."""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from services.auc import auc, keystore                                   # noqa: E402
from services.auc.milenage import Milenage, sqn_to_bytes                  # noqa: E402

H = bytes.fromhex
K = '465b5ce8b199b49faa5f0a2ee238a6bc'
OPC = 'cd63cb71954a9f4e48a5994e37a02baf'


class FakeCursor:
    def __init__(self, db):
        self.db, self.rows, self.executed = db, [], []

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def execute(self, sql, params=()):
        self.executed.append((sql, params))
        s = sql.strip()
        if s.startswith("SHOW COLUMNS"):
            self.rows = [{"Field": "auth_scheme"}] if self.db.get("migrated", True) else []
        elif s.startswith("SELECT"):
            table = s.split(" FROM ")[1].split()[0]
            row = self.db["tables"].get(table, {}).get(params[0])
            self.rows = [dict(row)] if row else []
        elif s.startswith("UPDATE"):
            table = s.split()[1]
            self.db["tables"][table][params[1]]["sqn"] = params[0]
            self.rows = []

    def fetchone(self):
        return self.rows[0] if self.rows else None


class FakeConn:
    def __init__(self, db):
        self.db, self.began, self.committed, self.rolled = db, 0, 0, 0

    def begin(self):
        self.began += 1

    def commit(self):
        self.committed += 1

    def rollback(self):
        self.rolled += 1

    def cursor(self):
        return FakeCursor(self.db)


def _db(scheme='aka', sqn=0, migrated=True):
    kek = keystore.normalize_kek('00112233445566778899aabbccddeeff')
    return {"migrated": migrated, "tables": {
        "volte_subscriptions": {"+82100": {"auth_scheme": scheme, "k_enc": keystore.encrypt(kek, H(K)),
                                           "opc_enc": keystore.encrypt(kek, H(OPC)), "sqn": sqn,
                                           "amf": "8000", "imsi": "45033100"}},
        "ptt_subscriptions": {},
    }}


class TestAuc(unittest.TestCase):
    def setUp(self):
        auc.reset_schema_probe()
        auc.init({"AuC": {"Kek": "00112233445566778899aabbccddeeff"}, "InternalApi": {"Token": "tok"}})

    def test_issue_increments_sqn_and_av_is_consistent(self):
        db = _db(sqn=41)
        conn = FakeConn(db)
        out = auc.issue(conn, "+82100", "")
        self.assertEqual(out["scheme"], "aka")
        self.assertEqual(out["service"], "volte")
        self.assertEqual(db["tables"]["volte_subscriptions"]["+82100"]["sqn"], 42)
        self.assertEqual((conn.began, conn.committed, conn.rolled), (1, 1, 0))
        av = out["av"]
        rand, autn = H(av["rand"]), H(av["autn"])
        m = Milenage(H(K), H(OPC))
        res, ak = m.f2_f5(rand)
        self.assertEqual(res.hex(), av["xres"])
        sqn = bytes(a ^ b for a, b in zip(autn[:6], ak))
        self.assertEqual(sqn, sqn_to_bytes(42))
        self.assertEqual(autn[6:8].hex(), "8000")
        mac_a, _ = m.f1_f1star(rand, sqn, autn[6:8])
        self.assertEqual(mac_a, autn[8:])
        self.assertFalse(out["resynced"])
        for k_ in ("k", "opc", "k_enc"):
            self.assertNotIn(k_, out)   # 키 원문은 응답에 없다

    def test_resync_sets_sqn_from_auts(self):
        db = _db(sqn=7)
        conn = FakeConn(db)
        first = auc.issue(conn, "+82100", "volte")
        rand = H(first["av"]["rand"])
        m = Milenage(H(K), H(OPC))
        sqn_ms = sqn_to_bytes(9000)
        _, mac_s = m.f1_f1star(rand, sqn_ms, b"\x00\x00")
        auts = bytes(a ^ b for a, b in zip(sqn_ms, m.f5star(rand))) + mac_s
        out = auc.issue(conn, "+82100", "volte", rand.hex(), auts.hex())
        self.assertTrue(out["resynced"])
        self.assertEqual(db["tables"]["volte_subscriptions"]["+82100"]["sqn"], 9001)
        with self.assertRaises(auc.AucError) as cm:
            auc.issue(conn, "+82100", "volte", rand.hex(), ("00" * 14))
        self.assertEqual(cm.exception.status, 422)

    def test_error_mapping(self):
        with self.assertRaises(auc.AucError) as cm:
            auc.issue(FakeConn(_db()), "+82999", "")
        self.assertEqual(cm.exception.status, 404)
        with self.assertRaises(auc.AucError) as cm:
            auc.issue(FakeConn(_db(scheme='digest')), "+82100", "")
        self.assertEqual((cm.exception.status, cm.exception.code), (409, "scheme_mismatch"))
        auc.reset_schema_probe()
        with self.assertRaises(auc.AucError) as cm:
            auc.issue(FakeConn(_db(migrated=False)), "+82100", "")
        self.assertEqual(cm.exception.status, 503)
        # KEK 가 바뀌면 보관 키를 못 푼다 → 500
        auc.reset_schema_probe()
        auc.init({"AuC": {"Kek": "ffffffffffffffffffffffffffffffff"}, "InternalApi": {"Token": "tok"}})
        with self.assertRaises(auc.AucError) as cm:
            auc.issue(FakeConn(_db()), "+82100", "")
        self.assertEqual(cm.exception.status, 500)
        # KEK 미설정 → 503 (프로비저닝도)
        auc.init({})
        self.assertFalse(auc.enabled())
        with self.assertRaises(auc.AucError) as cm:
            auc.provision_keys(K, OPC)
        self.assertEqual(cm.exception.status, 503)

    def test_provision_keys(self):
        k_enc, opc_enc = auc.provision_keys(K, "", "cdc202d5123e20f62b6d676ac72cb318")   # op → OPc 유도
        k, opc = auc.decrypt_keys(k_enc, opc_enc)
        self.assertEqual((k.hex(), opc.hex()), (K, OPC))
        with self.assertRaises(auc.AucError):
            auc.provision_keys("zz", OPC)
        with self.assertRaises(auc.AucError):
            auc.provision_keys(K)
        self.assertEqual(auc.parse_amf(None), "8000")
        self.assertEqual(auc.parse_amf("B9B9"), "b9b9")
        with self.assertRaises(auc.AucError):
            auc.parse_amf("12345")

    def test_handler_token_gate(self):
        from handlers.auc_api import handle_av
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="POST", full_path="/internal/aka/av", client_ip="127.0.0.1", client_port=1,
                         headers={"authorization": "Bearer wrong"}, body={"msisdn": "+82100"})
        r = asyncio.run(handle_av(ha, {"config": {}}))
        self.assertEqual(r.status, 401)
        ha.headers = {"Authorization": "Bearer tok"}
        ha.body = {}
        r = asyncio.run(handle_av(ha, {"config": {}}))
        self.assertEqual(r.status, 400)
        ha.method = "GET"
        self.assertEqual(asyncio.run(handle_av(ha, {"config": {}})).status, 405)


if __name__ == '__main__':
    unittest.main()
