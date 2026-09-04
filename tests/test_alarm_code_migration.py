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
  - 정의 코드 정합: 코드에서 쓰는 전 코드가 알람 카탈로그 CSV(정본)의 정의 행에 존재.
  - 카탈로그 CSV 불변식(alarm_catalog.md §8): 정의 code 유일, 감지 code 의 정의 존재,
    정의-감지 블록 연속 배치·구분 일치, 알람 감지 행 활성키 후보 유일.

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


_CATALOG_CSV = os.path.join(_REPO, 'docs', 'design', 'alarm_catalog.csv')


def _catalog_rows():
    with open(_CATALOG_CSV, encoding='utf-8') as f:
        return list(csv.DictReader(f))


def _catalog_codes():
    return {row['code'] for row in _catalog_rows() if row['행'] == '정의'}


class TestCodeRevisions(unittest.TestCase):
    def test_all_legacy_codes_map_to_catalog(self):
        cat = _catalog_codes()
        for old, new in service_registry._CODE_REVISIONS.items():
            self.assertTrue(old.startswith('CIMS-'), old)
            self.assertIn(new, cat, f"{old}→{new} 가 카탈로그 정의 행에 없음")

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

    def test_check_rename_service_to_process_unresponsive(self):
        """구 check/type 'service_unresponsive' → 'process_unresponsive' 개명 이행."""
        out = service_registry.normalize_alert_rule(
            {'check': 'service_unresponsive', 'type': 'service_unresponsive',
             'code': 'A-PRC-004', 'target': 'csp'})
        self.assertEqual(out['check'], 'process_unresponsive')
        self.assertEqual(out['type'], 'process_unresponsive')
        self.assertEqual(out['code'], 'A-PRC-004')
        self.assertEqual(out['target'], 'csp')
        # check 없이 구 type 만 있는 레코드/규칙도 alias 로 보정
        out = service_registry.normalize_alert_rule({'type': 'service_unresponsive'})
        self.assertEqual(out['type'], 'process_unresponsive')
        self.assertEqual(out['code'], 'A-PRC-004')

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


class TestMoRootImmutableId(unittest.TestCase):
    """mo 루트는 항상 불변 id — 주소·이름이 루트로 새면 활성키가 갈려 영구 미해소가 된다.

    실측 사고: 같은 CSP probe 조건이 `127.0.0.1/csp`(CspNotify 미설정 기본값)·
    `121.161.164.141/csp`(VIP, 그룹 등록 전)·`g2/csp`(그룹 등록 후) 세 키로 갈려,
    앞의 둘이 닫히지 않고 콘솔에 계속 떠 있었다.
    """

    def test_resolver_returns_empty_when_unresolved(self):
        # 스토어가 비면 해석 불가 — 주소를 루트로 돌려주지 않는다.
        resolve = alarm_sweeper.build_mo_root_resolver({})
        self.assertEqual(resolve('121.161.164.141'), '')
        self.assertEqual(resolve('127.0.0.1'), '')

    def test_owner_root_falls_back_to_mgmt_not_address(self):
        cfg = {'SystemId': 'oam1'}
        resolve = alarm_sweeper.build_mo_root_resolver(cfg)
        root = alarm_sweeper.owner_mo_root(cfg, resolve, '121.161.164.141', 'csp')
        self.assertEqual(root, 'oam1')          # 관리평면 루트 — 주소가 아니다
        self.assertFalse(alarm_sweeper._is_addr_root(root))

    def test_addr_root_detection(self):
        for r in ('127.0.0.1', '121.161.164.141', '10.0.0.1:9000'):
            self.assertTrue(alarm_sweeper._is_addr_root(r), r)
        for r in ('g2', 'a1', 'oam', 'cims'):
            self.assertFalse(alarm_sweeper._is_addr_root(r), r)

    def test_mo_root_of(self):
        self.assertEqual(alarm_sweeper.mo_root_of('A-PRC-004@g2/csp'), 'g2')
        self.assertEqual(alarm_sweeper.mo_root_of('A-PRC-004@127.0.0.1/csp'), '127.0.0.1')

    def test_drift_mo_root_prefers_id_over_name(self):
        from services import drift_sweeper
        r = {'ha_group_id': 2, 'ha_group_name': '제어그룹', 'collection': 'local_nodes'}
        self.assertEqual(drift_sweeper._mo_instance(r), 'g2/config/local_nodes')
        # 이름이 바뀌어도 키는 그대로 — 열린 알람을 계속 찾을 수 있다.
        r2 = {**r, 'ha_group_name': '이름변경'}
        self.assertEqual(drift_sweeper._mo_instance(r2), drift_sweeper._mo_instance(r))


class TestStaleCleanupGuards(unittest.TestCase):
    """신원 재해석 정리는 **close 를 발행**한다 — 근거 없이 돌면 살아 있는 알람을 지우거나,
    지웠다 열었다 하는 플래핑이 된다. 두 가드가 지켜지는지 본다.
      ① 관측 실패(probe 무응답) → 손대지 않는다
      ② 신원 미해석(인벤토리 읽기 실패로 주소가 그대로 루트) → 손대지 않는다
    한 번 사라졌던 가드다(상류 병합에서 조용히 빠졌다) — 시험으로 못 박아 둔다."""

    RULE = {'type': 'process_unresponsive', 'code': 'A-PRC-004', 'check': 'process_unresponsive',
            'target': 'csp', 'perceived_severity': 'major', 'mo_class': 'service',
            'msg_open': '{mo} 무응답', 'msg_close': '{mo} 정상화'}

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='alarmguard_')
        self.config = {'CspNotify': {'Ip': '10.0.0.9'}}
        from handlers import stats as _stats
        from services import service_registry as _reg
        self._saved = {'csp': _stats._get_csp_stats, 'db': _stats._get_db,
                       'ep': _stats._media_endpoints, 'rules': _reg.alert_rules,
                       'resolver': alarm_sweeper.build_mo_root_resolver}
        self._stats, self._reg = _stats, _reg
        _stats._media_endpoints = lambda _c: []
        _stats._get_db = lambda _c: (_ for _ in ()).throw(RuntimeError('db 없음'))
        _reg.alert_rules = lambda _c: [dict(self.RULE)]

    def tearDown(self):
        self._stats._get_csp_stats = self._saved['csp']
        self._stats._get_db = self._saved['db']
        self._stats._media_endpoints = self._saved['ep']
        self._reg.alert_rules = self._saved['rules']
        alarm_sweeper.build_mo_root_resolver = self._saved['resolver']
        shutil.rmtree(self.dir, ignore_errors=True)

    def _sweep(self, stale_mo: str, *, observable: bool, resolved: bool) -> dict:
        """옛 루트로 열린 활성키 하나를 두고 한 바퀴 돌린 뒤의 state."""
        self._stats._get_csp_stats = lambda _c: ({'uptime': 1} if observable else {})
        # resolved=True → 인벤토리가 주소를 불변 id 로 해석. False → 주소를 그대로 루트로 쓴다.
        alarm_sweeper.build_mo_root_resolver = (
            lambda _c: (lambda a: 'a7' if resolved else str(a)))
        state = {}
        alarm_sweeper.transition(state, self.dir, dict(self.RULE), stale_mo,
                                 'oam-svc', True, 'open', 'close')
        self.assertIn(f'A-PRC-004@{stale_mo}', state)       # 전제: 옛 루트로 열려 있다
        alarm_sweeper.sweep_service_rules(self.config, state, self.dir, 'oam-svc')
        return state

    # ① 관측 실패 — 옛 **불변 id** 루트(단일 인스턴스 분기 소관)
    def test_keeps_stale_when_unobservable(self):
        state = self._sweep('a9/csp', observable=False, resolved=True)
        self.assertIn('A-PRC-004@a9/csp', state,
                      '관측 실패 구간에서 옛 활성키를 오종결했다')

    def test_closes_stale_when_observable(self):
        state = self._sweep('a9/csp', observable=True, resolved=True)
        self.assertNotIn('A-PRC-004@a9/csp', state,
                         '근거가 갖춰졌는데 옛 활성키가 안 닫혔다')

    # ② 신원 미해석 — **주소** 루트(이행 종결 블록 소관). 이때 주소 루트는 폴백이 아니라
    #    현행 형태라, 닫으면 다음 평가가 다시 열어 플래핑이 된다.
    def test_keeps_addr_root_when_identity_unresolved(self):
        state = self._sweep('10.0.0.1/csp', observable=True, resolved=False)
        self.assertIn('A-PRC-004@10.0.0.1/csp', state,
                      '신원 미해석 상태에서 주소 루트를 종결해 플래핑을 만든다')

    def test_closes_addr_root_when_identity_resolves(self):
        state = self._sweep('10.0.0.1/csp', observable=True, resolved=True)
        self.assertNotIn('A-PRC-004@10.0.0.1/csp', state,
                         '신원이 해석되는데 구 주소 루트가 안 닫혔다')


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


class TestSipStatsSelfReport(unittest.TestCase):
    """CSP SIP 통계 자기보고 — 카탈로그 선언 + 단계 severity 경로 (fm_ingest).

    payload perceived_severity 우선(자기보고 §4)과 승격 시 action=change
    (표준화 §3.4(d)), FM_SYNC reconcile 의 단계 보존을 검증한다."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='fm_sipstats_')

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def _ingest(self):
        from services.fm_ingest import FmIngest
        ing = FmIngest({'FmIngest': {}}, self.dir)
        node = 'SIG_SVR_01'
        ing.catalogs[node] = ing._index_catalog({
            'node': node, 'module': 'csp',
            'alarms': [{'code': 'A-QOS-006', 'type': 'threshold_crossed',
                        'perceived_severity': 'minor',
                        'msg_open': 'Call success rate is {observed}%',
                        'msg_close': 'recovered'}]})
        ent = ing.nodes.setdefault(node, {'boot_id': 1, 'module': 'csp',
                                          'akeys': set(), 'seq': {}, 'last_sync': 0})
        return ing, node, ent

    def test_csp_fm_catalog_declares_sip_stats_codes(self):
        p = os.path.join(_REPO, 'csp', 'config', 'fm_catalog.json')
        with open(p, encoding='utf-8') as f:
            d = json.load(f)
        codes = {a['code']: a for a in d['alarms']}
        # 클래스는 code 별로 갈린다 — 분류 축은 매체가 아니라 잃은 능력(표준화 §3.5).
        #   성공률 저하는 품질, 수용 상한 접근은 용량, 그 외 단순 임계는 threshold_crossed.
        for c, t in (('A-QOS-006', 'quality_degraded'),
                     ('A-QOS-007', 'quality_degraded'),
                     ('A-QOS-009', 'capacity_threshold'),
                     ('A-QOS-011', 'threshold_crossed')):
            self.assertIn(c, codes, f"csp fm_catalog 에 {c} 미선언")
            self.assertEqual(codes[c]['type'], t, c)
        for c in ('A-SEC-003', 'A-SEC-004'):
            self.assertIn(c, codes, f'csp fm_catalog 에 {c} 미선언')
            self.assertEqual(codes[c]['type'], 'security_violation', c)

    def test_payload_severity_and_escalation_change(self):
        ing, node, ent = self._ingest()
        mo = f'{node}/csp/calls/success_rate'
        akey = f'A-QOS-006@{mo}'
        rule = ing.catalogs[node]['alarms']['A-QOS-006']
        ing._transition(node, ent, rule, mo, True,
                        params={'observed': '85.0'}, severity='minor')
        self.assertEqual(ing.state[akey]['severity'], 'minor')
        ing._transition(node, ent, rule, mo, True,
                        params={'observed': '60.0'}, severity='major')
        self.assertEqual(ing.state[akey]['severity'], 'major')
        recs = [r for r in alert_log.read_recent(self.dir, days=1)
                if r.get('alarm_id', '').startswith(akey)]
        self.assertEqual(sorted(r['action'] for r in recs), ['change', 'open'])
        chg = next(r for r in recs if r['action'] == 'change')
        self.assertEqual(chg['trend_indication'], 'moreSevere')
        self.assertEqual(chg['perceived_severity'], 'major')

    def test_reconcile_preserves_staged_severity(self):
        ing, node, ent = self._ingest()
        mo = f'{node}/csp/calls/success_rate'
        akey = f'A-QOS-006@{mo}'
        ing._reconcile(node, ent, [{'code': 'A-QOS-006', 'mo_instance': mo,
                                    'perceived_severity': 'critical',
                                    'params': {'observed': '40.0'}}])
        self.assertEqual(ing.state[akey]['severity'], 'critical')


class TestCatalogCsvInvariants(unittest.TestCase):
    """alarm_catalog.csv 불변식 (alarm_catalog.md §8)."""

    # 의도적 활성키 공유: 상보 감지 경로 (alarm_catalog.md §3.2)
    _ACTIVE_KEY_EXCEPTION = ('storage_failure', 'CMDP', '', 'fd_store')

    def test_definition_codes_unique(self):
        codes = [r['code'] for r in _catalog_rows() if r['행'] == '정의']
        dup = {c for c in codes if codes.count(c) > 1}
        self.assertFalse(dup, f"정의 code 중복: {sorted(dup)}")

    def test_detection_codes_have_definition(self):
        defs = _catalog_codes()
        for r in _catalog_rows():
            if r['행'] == '감지':
                self.assertIn(r['code'], defs, f"감지 행 {r['code']} 의 정의 없음")

    def test_detection_block_follows_definition(self):
        """감지 행은 자기 정의 행 바로 아래 연속 블록 + 구분 일치."""
        cur = None
        for i, r in enumerate(_catalog_rows(), 2):
            if r['행'] == '정의':
                cur = (r['code'], r['구분'])
            else:
                self.assertIsNotNone(cur, f"행 {i}: 정의 행 없이 감지 행")
                self.assertEqual(r['code'], cur[0],
                                 f"행 {i}: 감지 {r['code']} 가 정의 {cur[0]} 블록 밖")
                self.assertEqual(r['구분'], cur[1], f"행 {i}: 구분 불일치")

    def test_alarm_detection_active_key_candidates_unique(self):
        """자기보고 모듈(L2)의 알람 감지 행 (instance, 대상, component) 유일.

        AGENT/OAM 행은 sweeper 합성 규칙(§7)이라 불변식 밖 — 같은 정의의 원인 축을
        행으로 분리한다(OAM A-PRC-003 config/<collection> 4행).
        """
        seen = {}
        for i, r in enumerate(_catalog_rows(), 2):
            if r['행'] != '감지' or r['구분'] != '알람':
                continue
            if r['instance'] not in ('CSP', 'CMP', 'CMDP', 'CSC'):
                continue
            key = (r['type'], r['instance'], r['대상'], r['component'])
            if key == self._ACTIVE_KEY_EXCEPTION:
                continue
            self.assertNotIn(key, seen, f"행 {i}: 활성키 후보 충돌 {key} (행 {seen.get(key)})")
            seen[key] = i


if __name__ == '__main__':
    unittest.main(verbosity=2)
