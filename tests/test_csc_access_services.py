"""csc/src/services/access_services.py — 접속 서비스 단일 읽기 경로 단위 시험 (오프라인, DB 없음).

sip_service_model.md §2-9: 정의(name·kind·domain·auth_realm·media_srtp·sec_mechanisms·피처코드)는 CSP access_services 의
관리 store 미러가 정본, csc.json `Provisioning.Services.<kind>` 는 단말 도달 정보(host·포트·transport)를 보탠다. 미러가 없으면
csc.json 만으로. 미러는 tmp runtime store 에 파일로 쓴다(ha_lookup 비표준 레이아웃 = {runtime}/collections/csp/access_services).

  python3 -m unittest tests.test_csc_access_services
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

from services import access_services as acc  # noqa: E402

MIRROR = [
    {"id": "a1", "name": "volte", "kind": "volte", "domain": "volte.sot", "auth_realm": "", "enabled": True, "priority": 100,
     "media_srtp": "optional", "sec_mechanisms": ["tls"]},
    {"id": "a2", "name": "voip", "kind": "voip", "domain": "voip.sot", "auth_realm": "voip.realm", "enabled": True, "priority": 150,
     "media_srtp": "required", "sec_mechanisms": ["tls"], "pickup_feature_code": "**", "transfer_allowed": True},
    {"id": "a3", "name": "mcptt", "kind": "ptt", "domain": "ptt.sot", "enabled": True, "priority": 100, "media_srtp": "off"},
    {"id": "a4", "name": "volte-old", "kind": "volte", "domain": "old.sot", "enabled": False, "priority": 10},   # disabled
    {"id": "a5", "name": "no-domain", "kind": "volte", "domain": "", "enabled": True, "priority": 1},           # domain 없음
]
PROV = {"Services": {
    "volte": {"name": "volte", "domain": "volte.cfg", "host": "10.0.0.1", "port": 15060, "tls_port": 15061, "transport": "UDP",
              "media_srtp": "off"},
    "voip": {"name": "voip", "domain": "voip.sot", "host": "10.0.0.2", "port": 5060, "tls_port": 5061, "transport": "TLS"},
    "ptt": {"name": "ptt", "domain": "ptt.cfg", "host": "10.0.0.3", "port": 15060, "tls_port": 15061, "sms_gateway": True},
}}


class _Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="csc_acc_")
        self.rt = os.path.join(self.tmp, "rt")
        self.cfg = {"CimsRuntimeDir": self.rt}
        acc.configure(self.cfg)

    def tearDown(self):
        acc.configure({})
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_mirror(self, rows=MIRROR):
        d = os.path.join(self.rt, "collections", "csp", "access_services")
        os.makedirs(d, exist_ok=True)
        for r in rows:
            with open(os.path.join(d, f"{r['id']}.json"), "w", encoding="utf-8") as f:
                json.dump(r, f)


class RecordsTests(_Base):
    def test_records_filter_and_order(self):
        self.write_mirror()
        names = [r["name"] for r in acc.records(self.cfg)]
        self.assertEqual(names, ["mcptt", "volte", "voip"])          # priority 100,100(name 순),150 — disabled·domain 없음 제외

    def test_records_missing_mirror_is_empty(self):
        self.assertEqual(acc.records(self.cfg), [])
        self.assertEqual(acc.records(), [])                            # configure() 된 설정으로

    def test_family(self):
        self.assertEqual((acc.family("volte"), acc.family("voip"), acc.family("ptt"), acc.family("mcptt"), acc.family("x")),
                         ("phone", "phone", "ptt", "ptt", ""))


class FindResolveTests(_Base):
    def setUp(self):
        super().setUp()
        self.write_mirror()

    def test_find_by_name_within_family(self):
        self.assertEqual(acc.find(self.cfg, "voip", "volte")["domain"], "voip.sot")      # 전화 가족 안: volte 회선이 voip 서비스
        self.assertEqual(acc.find(self.cfg, "mcptt", "ptt")["domain"], "ptt.sot")
        self.assertIsNone(acc.find(self.cfg, "voip", "ptt"))                             # 가족 경계는 넘지 않는다
        self.assertIsNone(acc.find(self.cfg, "mcptt", "volte"))
        self.assertIsNone(acc.find(self.cfg, "", "volte"))
        self.assertIsNone(acc.find(self.cfg, "volte-old", "volte"))                      # disabled

    def test_pick_by_kind_priority_and_alias(self):
        self.assertEqual(acc.pick_by_kind(self.cfg, "volte")["name"], "volte")
        self.assertEqual(acc.pick_by_kind(self.cfg, "voip")["name"], "voip")
        self.assertEqual(acc.pick_by_kind(self.cfg, "ptt")["name"], "mcptt")
        self.assertEqual(acc.pick_by_kind(self.cfg, "mcptt")["name"], "mcptt")
        self.assertIsNone(acc.pick_by_kind(self.cfg, "ibcf"))

    def test_resolve_name_then_kind(self):
        self.assertEqual(acc.resolve(self.cfg, "voip", "volte")["name"], "voip")
        self.assertEqual(acc.resolve(self.cfg, "", "volte")["name"], "volte")            # service_ref 없음 → kind
        self.assertEqual(acc.resolve(self.cfg, "no-such", "ptt")["name"], "mcptt")      # 미지 이름 → kind


class EntryTests(_Base):
    def test_entry_merges_identity_over_reachability(self):
        self.write_mirror()
        kind, svc = acc.entry("volte", "voip", PROV)
        self.assertEqual(kind, "voip")
        self.assertEqual((svc["domain"], svc["auth_realm"], svc["media_srtp"], svc["pickup_feature_code"]),
                         ("voip.sot", "voip.realm", "required", "**"))                  # 정의 = 미러
        self.assertEqual((svc["host"], svc["port"], svc["tls_port"], svc["transport"]), ("10.0.0.2", 5060, 5061, "TLS"))  # 도달 = csc.json

    def test_entry_ptt_name_mismatch_is_harmless(self):
        # csc.json 키 'ptt'/name 'ptt' vs CSP name 'mcptt' — 레코드 kind 로 항목을 고르므로 이름이 달라도 합쳐진다
        self.write_mirror()
        kind, svc = acc.entry("ptt", "mcptt", PROV)
        self.assertEqual((kind, svc["domain"], svc["name"], svc["host"], svc["sms_gateway"]),
                         ("ptt", "ptt.sot", "mcptt", "10.0.0.3", True))

    def test_entry_drift_uses_mirror_and_warns_once(self):
        self.write_mirror()
        warned = []
        saved = acc.logger.log_warning
        acc.logger.log_warning = lambda m: warned.append(m)
        try:
            for _ in range(3):
                kind, svc = acc.entry("volte", "volte", PROV)
                self.assertEqual((kind, svc["domain"], svc["media_srtp"]), ("volte", "volte.sot", "optional"))
        finally:
            acc.logger.log_warning = saved
        self.assertEqual(len(warned), 1)
        self.assertIn("volte.cfg", warned[0])

    def test_entry_without_mirror_falls_back_to_config(self):
        kind, svc = acc.entry("volte", "voip", PROV)                  # 이름 매칭(가족 안)
        self.assertEqual((kind, svc["domain"]), ("voip", "voip.sot"))
        kind, svc = acc.entry("volte", "", PROV)                      # 종류 키
        self.assertEqual((kind, svc["domain"]), ("volte", "volte.cfg"))
        kind, svc = acc.entry("ptt", "voip", PROV)                    # 가족 경계
        self.assertEqual((kind, svc["domain"]), ("ptt", "ptt.cfg"))
        self.assertEqual(acc.entry("ptt", "x", {}), ("ptt", {}))

    def test_entry_mirror_without_config_entry(self):
        self.write_mirror()
        kind, svc = acc.entry("volte", "voip", {"Services": {"volte": {"host": "h", "port": 1}}})
        self.assertEqual((kind, svc["domain"], svc["host"]), ("voip", "voip.sot", "h"))     # 가족 키(volte) 항목 폴백
        kind, svc = acc.entry("volte", "voip", {})
        self.assertEqual((kind, svc["domain"], svc.get("host")), ("voip", "voip.sot", None))


class PttDomainTests(_Base):
    def test_ptt_domain_mirror_then_config(self):
        self.assertEqual(acc.ptt_domain(PROV), "ptt.cfg")
        self.write_mirror()
        self.assertEqual(acc.ptt_domain(PROV), "ptt.sot")
        self.assertEqual(acc.ptt_domain({}), "ptt.sot")
        acc.configure({"CimsRuntimeDir": os.path.join(self.tmp, "none")})
        self.assertEqual(acc.ptt_domain({}), "")


if __name__ == "__main__":
    unittest.main()
