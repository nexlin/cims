"""
알람 정의 코드(A-*)·mo 소유 주체 루트 이행 단위 테스트 (표준화 §3.4·§6).

Covers (모듈 직접 호출 — 서버 미기동):
  - service_registry: 코드 개정 매핑(_CODE_REVISIONS)·current_code alias,
    구 포맷 규칙(code CIMS-*, mo cims/*) read 정규화 — check 기반 정의 코드 분할
    (disk=A-QOS-001 / ha_flap=A-QOS-023 / rtp=A-QOS-024) 포함.
  - alarm_sweeper: detected_by 소유 파티션 판정(partition_of — 파이프라인 §4.3),
    restore_open_state scope 분리, transition 의 detected_by 동반 상태,
    close_migrated_keys 이행 종결.
  - fm_ingest: 구 wire mo(cims/<module>/<node>[/...]) 정규화, 복원 시 서버명 루트
    node 도출, 구 코드 카탈로그 인덱스 alias.
  - 정의 코드 정합: 코드에서 쓰는 전 코드가 기능 카탈로그 CSV(정본)에 존재.

sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
import csv
import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

from services import alarm_sweeper, alert_log, service_registry  # noqa: E402
from services.fm_ingest import _current_code, _normalize_mo      # noqa: E402


def _catalog_codes():
    path = os.path.join(_REPO, 'docs', 'design', 'alarm_function_catalog.csv')
    with open(path, encoding='utf-8') as f:
        return {row['code'] for row in csv.DictReader(f)}


class TestCodeRevisions(unittest.TestCase):
    def test_all_legacy_codes_map_to_catalog(self):
        cat = _catalog_codes()
        for old, new in service_registry._CODE_REVISIONS.items():
            self.assertTrue(old.startswith('CIMS-'), old)
            self.assertIn(new, cat, f"{old}→{new} 가 기능 카탈로그에 없음")

    def test_current_code_alias(self):
        self.assertEqual(service_registry.current_code('CIMS-COM-001'), 'A-COM-001')
        self.assertEqual(service_registry.current_code('CIMS-CFG-001'), 'A-PRC-003')
        self.assertEqual(service_registry.current_code('A-QOS-002'), 'A-QOS-002')

    def test_rule_defaults_use_catalog_codes(self):
        cat = _catalog_codes()
        for chk, d in service_registry._ALERT_CLASS_DEFAULTS.items():
            self.assertIn(d['code'], cat, f"check={chk} 기본 code {d['code']}")
        for r in service_registry._CORE_ALERT_RULES:
            self.assertIn(r['code'], cat, f"core rule {r.get('check')}")

    def test_qos001_split_by_check(self):
        """구 CIMS-QOS-001 이 3정의로 분할 — 저장된 구 코드는 check 기본값이 배정."""
        for chk, want in [('disk_high', 'A-QOS-001'), ('ha_flap', 'A-QOS-023'),
                          ('rtp_pct_gte', 'A-QOS-024')]:
            out = service_registry.normalize_alert_rule(
                {'check': chk, 'code': 'CIMS-QOS-001', 'type': 'threshold_crossed'})
            self.assertEqual(out['code'], want, chk)

    def test_legacy_mo_instance_dropped(self):
        out = service_registry.normalize_alert_rule(
            {'check': 'db_down', 'code': 'CIMS-COM-001', 'mo_instance': 'cims/db'})
        self.assertEqual(out['code'], 'A-COM-001')
        self.assertNotIn('mo_instance', out)
        # 신 포맷 명시값은 유지
        out2 = service_registry.normalize_alert_rule(
            {'check': 'db_down', 'mo_instance': 'MGMT_G1/db'})
        self.assertEqual(out2['mo_instance'], 'MGMT_G1/db')


class TestPartition(unittest.TestCase):
    def test_partition_by_detected_by(self):
        p = alarm_sweeper.partition_of
        self.assertEqual(p('self', 'A-COM-001@N1/csp/db'), 'self')
        self.assertEqual(p('self:CSP_01', 'CIMS-COM-001@cims/csp/CSP_01/db'), 'self')
        self.assertEqual(p('agent', 'A-PRC-001@N1/csp'), 'agent')
        self.assertEqual(p('oam-svc', 'A-PRC-004@N1/csp'), 'service')
        self.assertEqual(p('oam', 'A-PRC-003@g1/config/routes'), 'service')
        # detected_by 없는 구 레코드 — mo 접두 폴백
        self.assertEqual(p('', 'CIMS-PRC-004@cims/csp'), 'service')
        self.assertEqual(p('', 'CIMS-QOS-001@host1/disk'), 'agent')


class TestRestoreAndMigration(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='alarmtest_')

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def _open(self, code, mo, detected_by, sev='major'):
        rule = {'code': code, 'type': 't', 'perceived_severity': sev}
        st = {}
        alarm_sweeper.transition(st, self.dir, rule, mo, detected_by, True, 'open', 'close')
        return st

    def test_transition_state_carries_detected_by(self):
        st = self._open('A-PRC-004', 'N1/csp', 'oam-svc')
        ent = st['A-PRC-004@N1/csp']
        self.assertEqual(ent['detected_by'], 'oam-svc')

    def test_restore_scope_by_partition(self):
        self._open('A-PRC-001', 'N1/csp', 'agent')
        self._open('A-PRC-004', 'N1/csp', 'oam-svc')
        self._open('A-COM-001', 'N1/csp/db', 'self')
        ag = alarm_sweeper.restore_open_state(self.dir, scope='agent')
        sv = alarm_sweeper.restore_open_state(self.dir, scope='service')
        se = alarm_sweeper.restore_open_state(self.dir, scope='self')
        al = alarm_sweeper.restore_open_state(self.dir, scope='all')
        self.assertEqual(sorted(ag), ['A-PRC-001@N1/csp'])
        self.assertEqual(sorted(sv), ['A-PRC-004@N1/csp'])
        self.assertEqual(sorted(se), ['A-COM-001@N1/csp/db'])
        self.assertEqual(sorted(al), ['A-PRC-001@N1/csp', 'A-PRC-004@N1/csp'])

    def test_close_migrated_keys(self):
        st = self._open('CIMS-PRC-004', 'cims/csp', 'oam-svc')
        st.update(self._open('A-PRC-004', 'N1/csp', 'oam-svc'))
        n = alarm_sweeper.close_migrated_keys(
            st, self.dir, 'oam-svc',
            lambda k: '@cims/' in k, '이행 종결')
        self.assertEqual(n, 1)
        self.assertEqual(sorted(st), ['A-PRC-004@N1/csp'])
        # close 레코드가 남아 replay 에서도 닫힘
        state = alert_log.compute_open_state(self.dir)
        self.assertNotIn('CIMS-PRC-004@cims/csp', state)


class TestRetentionPurge(unittest.TestCase):
    def test_purge_old_by_file_date(self):
        from services import daily_jsonl
        from datetime import datetime, timedelta
        d = tempfile.mkdtemp(prefix='rettest_')
        try:
            old = datetime.now() - timedelta(days=200)
            new = datetime.now()
            for dt in (old, new):
                daily_jsonl.record(d, 'alerts', {'ts': dt.isoformat(timespec='seconds'),
                                                 'type': 't', 'action': 'open'})
            self.assertEqual(daily_jsonl.purge_old(d, 'alerts', 180), 1)
            self.assertEqual(daily_jsonl.purge_old(d, 'alerts', 0), 0)   # 0 = 무제한
            remain = list(daily_jsonl.iter_asc(d, 'alerts', days=1))
            self.assertEqual(len(remain), 1)
        finally:
            shutil.rmtree(d, ignore_errors=True)


class TestAgentLostRule(unittest.TestCase):
    def test_core_rule_registered(self):
        r = next((r for r in service_registry._CORE_ALERT_RULES
                  if r.get('check') == 'agent_lost'), None)
        self.assertIsNotNone(r)
        self.assertEqual(r['code'], 'A-COM-015')
        self.assertEqual(r['type'], 'connection_lost')
        self.assertEqual(r['scope'], 'agent')
        self.assertIn('A-COM-015', _catalog_codes())


class TestFmWireNormalize(unittest.TestCase):
    def test_normalize_mo(self):
        self.assertEqual(_normalize_mo('cims/csp/CSP_01/db'), 'CSP_01/csp/db')
        self.assertEqual(_normalize_mo('cims/cmp/MED_01'), 'MED_01/cmp')
        self.assertEqual(_normalize_mo('cims/cmp/MED_01/rtp_pool'), 'MED_01/cmp/rtp_pool')
        self.assertEqual(_normalize_mo('CSP_01/csp/db'), 'CSP_01/csp/db')   # 현행 통과
        self.assertIsNone(_normalize_mo(None))

    def test_current_code_wire_alias(self):
        self.assertEqual(_current_code('CIMS-QOS-002'), 'A-QOS-002')
        self.assertEqual(_current_code('A-PRC-002'), 'A-PRC-002')

    def test_index_catalog_aliases_legacy(self):
        from services.fm_ingest import FmIngest
        idx = FmIngest._index_catalog({
            'node': 'N1', 'module': 'cmdp',
            'alarms': [{'code': 'CIMS-PRC-002', 'type': 'resource_failure'}],
            'events': [{'type': 'process_started', 'code': 'E-STC-001'}]})
        self.assertIn('A-PRC-002', idx['alarms'])
        self.assertEqual(idx['alarms']['A-PRC-002']['type'], 'storage_failure')

    def test_fm_catalog_files_use_current_codes(self):
        cat = _catalog_codes()
        for m in ('csp', 'cmp', 'cmdp', 'csc'):
            p = os.path.join(_REPO, m, 'config', 'fm_catalog.json')
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
            for a in d.get('alarms', []):
                self.assertIn(a['code'], cat, f"{m} alarm {a['code']}")
            for e in d.get('events', []):
                self.assertIn(e.get('code'), cat, f"{m} event {e.get('type')}")


if __name__ == '__main__':
    unittest.main(verbosity=2)
