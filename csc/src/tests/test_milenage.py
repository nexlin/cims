"""Milenage / AES-128 / keystore 단위시험 — TS 35.207 §6 / TS 35.208 §4 시험 벡터."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from services.auc.aes128 import aes128_encrypt                                # noqa: E402
from services.auc.milenage import (Milenage, opc_from_op, generate_av, resync_sqn,   # noqa: E402
                                   sqn_to_bytes)
from services.auc import keystore                                              # noqa: E402

H = bytes.fromhex

# TS 35.208 §4 Test Sets (K, RAND, SQN, AMF, OP, OPc, f1, f1*, f2, f5, f3, f4, f5*)
_SETS = [
    dict(k='465b5ce8b199b49faa5f0a2ee238a6bc', rand='23553cbe9637a89d218ae64dae47bf35',
         sqn='ff9bb4d0b607', amf='b9b9', op='cdc202d5123e20f62b6d676ac72cb318',
         opc='cd63cb71954a9f4e48a5994e37a02baf', f1='4a9ffac354dfafb3', f1s='01cfaf9ec4e871e9',
         f2='a54211d5e3ba50bf', f5='aa689c648370', f3='b40ba9a3c58b2a05bbf0d987b21bf8cb',
         f4='f769bcd751044604127672711c6d3441', f5s='451e8beca43b'),
    dict(k='0396eb317b6d1c36f19c1c84cd6ffd16', rand='c00d603103dcee52c4478119494202e8',
         sqn='fd8eef40df7d', amf='af17', op='ff53bade17df5d4e793073ce9d7579fa',
         opc='53c15671c60a4b731c55b4a441c0bde2', f1='5df5b31807e258b0', f1s='a8c016e51ef4a343',
         f2='d3a628ed988620f0', f5='c47783995f72', f3='58c433ff7a7082acd424220f2b67c556',
         f4='21a8c1f929702adb3e738488b9f5c5da', f5s='30f1197061c1'),
    dict(k='fec86ba6eb707ed08905757b1bb44b8f', rand='9f7c8d021accf4db213ccff0c7f71a6a',
         sqn='9d0277595ffc', amf='725c', op='dbc59adcb6f9a0ef735477b7fadf8374',
         opc='1006020f0a478bf6b699f15c062e42b3', f1='9cabc3e99baf7281', f1s='95814ba2b3044324',
         f2='8011c48c0c214ed2', f5='33484dc2136b', f3='5dbdbb2954e8f3cde665b046179a5098',
         f4='59a92d3b476a0443487055cf88b2307b', f5s='deacdd848cc6'),
]


class TestAes(unittest.TestCase):
    def test_fips197(self):
        self.assertEqual(aes128_encrypt(H('000102030405060708090a0b0c0d0e0f'),
                                        H('00112233445566778899aabbccddeeff')).hex(),
                         '69c4e0d86a7b0430d8cdb78070b4c55a')


class TestMilenage(unittest.TestCase):
    def test_vectors(self):
        for i, v in enumerate(_SETS):
            with self.subTest(set=i + 1):
                k, rand, sqn, amf = H(v['k']), H(v['rand']), H(v['sqn']), H(v['amf'])
                self.assertEqual(opc_from_op(k, H(v['op'])).hex(), v['opc'])
                m = Milenage(k, H(v['opc']))
                mac_a, mac_s = m.f1_f1star(rand, sqn, amf)
                self.assertEqual(mac_a.hex(), v['f1'])
                self.assertEqual(mac_s.hex(), v['f1s'])
                res, ak = m.f2_f5(rand)
                self.assertEqual(res.hex(), v['f2'])
                self.assertEqual(ak.hex(), v['f5'])
                self.assertEqual(m.f3(rand).hex(), v['f3'])
                self.assertEqual(m.f4(rand).hex(), v['f4'])
                self.assertEqual(m.f5star(rand).hex(), v['f5s'])

    def test_av_and_resync(self):
        v = _SETS[0]
        k, opc, rand = H(v['k']), H(v['opc']), H(v['rand'])
        av = generate_av(k, opc, rand, H(v['sqn']), H(v['amf']))
        self.assertEqual(av['xres'].hex(), v['f2'])
        # AUTN = (SQN⊕AK) ‖ AMF ‖ MAC-A
        self.assertEqual(av['autn'][6:8].hex(), v['amf'])
        self.assertEqual(av['autn'][8:].hex(), v['f1'])
        self.assertEqual(bytes(a ^ b for a, b in zip(av['autn'][:6], H(v['f5']))).hex(), v['sqn'])
        # 단말이 만드는 AUTS = (SQN_MS ⊕ AK*) ‖ MAC-S(AMF*=0000) 를 서버가 되돌린다
        m = Milenage(k, opc)
        sqn_ms = sqn_to_bytes(0x123456789abc)
        _, mac_s = m.f1_f1star(rand, sqn_ms, b'\x00\x00')
        auts = bytes(a ^ b for a, b in zip(sqn_ms, m.f5star(rand))) + mac_s
        self.assertEqual(resync_sqn(k, opc, rand, auts), sqn_ms)
        self.assertIsNone(resync_sqn(k, opc, rand, auts[:-1] + b'\x00'))
        self.assertIsNone(resync_sqn(k, opc, rand, b'\x00' * 10))


class TestKeystore(unittest.TestCase):
    def test_roundtrip_and_tamper(self):
        kek = keystore.normalize_kek('00112233445566778899aabbccddeeff')
        stored = keystore.encrypt(kek, H(_SETS[0]['k']))
        self.assertTrue(stored.startswith('v1:'))
        self.assertEqual(len(stored), 3 + 32 + 32 + 64)
        self.assertEqual(keystore.decrypt(kek, stored).hex(), _SETS[0]['k'])
        # 변조·다른 KEK
        bad = stored[:-1] + ('0' if stored[-1] != '0' else '1')
        with self.assertRaises(ValueError):
            keystore.decrypt(kek, bad)
        with self.assertRaises(ValueError):
            keystore.decrypt(keystore.normalize_kek('ffffffffffffffffffffffffffffffff'), stored)
        # base64 KEK 도 정규화된다
        self.assertEqual(len(keystore.normalize_kek('vmM5AEeDY8j35qacD1UpxrqDcHpsJAq2tig9zcspDII=')), 16)


if __name__ == '__main__':
    unittest.main()
