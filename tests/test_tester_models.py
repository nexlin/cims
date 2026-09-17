"""계측기 계약 모델 단위시험 (ems/tester/oam/src/services/tester_models.py).

Covers:
  - 패키지 동봉 샘플(토폴로지·시나리오·프로파일) 전부 검증 통과
  - extra='forbid' — 오타 키·미지 지표 이름·미정의 역할 참조 거절
  - 프로파일 모델별 필수 필드
  - schema/*.json 이 모델과 동기화(bin/gen-schemas 산출과 동일)
"""
import glob
import json
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

import yaml  # noqa: E402
from services.tester_models import SCHEMAS, schema_json, validate  # noqa: E402


def _load(p):
    with open(p, 'r', encoding='utf-8') as f:
        return yaml.safe_load(f)


class BundledSamples(unittest.TestCase):
    def test_topology_sample(self):
        m, errs = validate('topology', _load(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml')))
        self.assertEqual(errs, [])
        self.assertEqual(m.pools['pbx_hq'].profile, 'pbx')
        self.assertEqual(m.pools['pbx_hq'].trunk_register.user, 'pbx-hq')   # YAML 키 register → alias

    def test_scenarios(self):
        files = [p for p in glob.glob(os.path.join(_TESTER, 'scenarios', '*', '*.yaml')) if '/profiles/' not in p]
        self.assertTrue(files)
        for p in files:
            m, errs = validate('scenario', _load(p))
            self.assertEqual(errs, [], p)
            self.assertTrue(m.flow)

    def test_profiles(self):
        files = glob.glob(os.path.join(_TESTER, 'scenarios', 'profiles', '*.yaml'))
        self.assertTrue(files)
        for p in files:
            _, errs = validate('profile', _load(p))
            self.assertEqual(errs, [], p)


class Strictness(unittest.TestCase):
    def _scn(self, **over):
        doc = {'id': 'T-BASIC', 'roles': {'a': {'pool': 'p'}, 'b': {'pool': 'p', 'disjoint_from': 'a'}},
               'flow': [{'step': 'register', 'who': ['a', 'b'], 'expect': {'code': 200}},
                        {'step': 'invite', 'from': 'a', 'to': 'b'},
                        {'step': 'bye', 'from': 'a', 'expect': {'sdd_ms': {'p95': 300}}}]}
        doc.update(over)
        return doc

    def test_typo_key_rejected(self):
        _, errs = validate('scenario', self._scn(flows=[]))
        self.assertTrue(any('flows' in e for e in errs))

    def test_unknown_metric_rejected(self):
        doc = self._scn()
        doc['flow'][0]['expect'] = {'rrd': {'p95': 1}}
        _, errs = validate('scenario', doc)
        self.assertTrue(any('알 수 없는 지표' in e for e in errs))

    def test_undefined_role_rejected(self):
        doc = self._scn()
        doc['flow'][1]['to'] = 'ghost'
        _, errs = validate('scenario', doc)
        self.assertTrue(any('ghost' in e for e in errs))

    def test_step_actor_required(self):
        doc = self._scn()
        doc['flow'].append({'step': 'answer'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('answer' in e for e in errs))

    def test_profile_required_by_model(self):
        _, errs = validate('profile', {'model': 'step', 'start': 5})
        self.assertTrue(any('step' in e and 'hold_s' in e for e in errs))
        m, errs = validate('profile', {'model': 'constant', 'rate': 4, 'duration_s': 60})
        self.assertEqual(errs, [])
        self.assertEqual(m.unit, 'saps')

    def test_unknown_kind(self):
        m, errs = validate('nope', {})
        self.assertIsNone(m)
        self.assertTrue(errs)

    def test_d_stage_steps(self):
        # dtmf 는 숫자열 payload 필수, cause 는 bye/reject 만, refer 는 from+to
        doc = self._scn()
        doc['flow'].append({'step': 'dtmf', 'from': 'a', 'payload': '12x'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('dtmf' in e for e in errs), errs)
        doc = self._scn()
        doc['flow'].append({'step': 'invite', 'from': 'a', 'to': 'b', 'cause': 16})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('cause' in e for e in errs), errs)
        doc = self._scn()
        doc['flow'].append({'step': 'refer', 'from': 'a'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('refer' in e for e in errs), errs)
        doc = self._scn()
        doc['flow'][2] = {'step': 'bye', 'from': 'a', 'cause': 16, 'expect': {'q850_rx_pct': 100, 'dtmf_rx_pct': 100}}
        doc['flow'].insert(2, {'step': 'progress', 'who': ['b'], 'after_ms': 100, 'expect': {'code': 183, 'early_media_pct': 100}})
        doc['flow'].insert(3, {'step': 'hold', 'who': ['b']})
        doc['flow'].insert(4, {'step': 'dtmf', 'from': 'a', 'payload': '12#A'})
        m, errs = validate('scenario', doc)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[-1].cause, 16)

    def test_ratio_metrics_are_metric_names(self):
        from services.tester_models import METRIC_NAMES, RATIO_METRICS
        for k in RATIO_METRICS:
            self.assertIn(k, METRIC_NAMES)

    def test_pool_options(self):
        m, errs = validate('topology', _load(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml')))
        self.assertEqual(errs, [])
        self.assertTrue(m.pools['volte_ue_a'].prack)
        self.assertEqual(m.pools['volte_ue_b'].group, 'volte_ue')
        # 파생 — 주소는 호스트에만: 워커 url·피어 수신점 ip·PoolCreate.target_csp·oam url
        self.assertEqual(m.worker_url(m.workers[1]), 'http://10.0.0.62:7100')
        self.assertEqual(m.pool_bind_ip('peer_kt'), '10.0.0.62')
        tc = m.target_csp_for('peer_kt')
        self.assertEqual((tc.ip, tc.udp, tc.tls, tc.domain_volte, tc.domain_ptt, tc.peering.port), ('10.0.0.45', 5060, 5061, 'volte.cims.example.kr', 'ptt.cims.example.kr', 5070))
        self.assertIsNone(m.target_csp_for('volte_ue_a').peering)
        self.assertEqual(m.oam_ref().url, 'https://10.0.0.45:4419')
        self.assertEqual((m.host_kind('h45'), m.host_kind('h61')), ('target', 'tester'))
        self.assertEqual(m.pools['mgcf_pstn'].profile, 'mgcf')
        self.assertEqual(m.pools['mgcf_pstn'].dial, 'number')
        self.assertEqual(m.pools['peer_kt'].dial, 'domain')
        self.assertIsNone(m.pools['mgcf_pstn'].prack)   # 프로파일 기본은 워커가 정한다
        # register 에는 ha1_env 또는 password_env
        doc = _load(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml'))
        doc['pools']['pbx_hq']['register'] = {'user': 'x'}
        _, errs = validate('topology', doc)
        self.assertTrue(any('ha1_env' in e for e in errs), errs)


class TopologyV2(unittest.TestCase):
    def _doc(self):
        return _load(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml'))

    def test_reference_checks(self):
        d = self._doc(); d['pools']['ptt_ue']['worker'] = 'w9'
        self.assertTrue(any('w9' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['ptt_ue']['access'] = 'cmp'
        self.assertTrue(any('access 수신점' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['ptt_ue']['transport'] = 'tls'; d['target']['nodes']['csp']['sip']['listeners'].pop('tls')
        self.assertTrue(any('tls access 수신점' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['peer_kt']['peering'] = 'cmp'
        self.assertTrue(any('수신점' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['peer_kt']['listener'] = 'nope'
        self.assertTrue(any('nope' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['volte_ue_b']['worker'] = 'w1'          # 같은 워커에 group 둘
        self.assertTrue(any('논리 풀 하나만' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['volte_ue_a']['group'] = 'ptt_ue'          # group = 다른 풀 이름
        self.assertTrue(any('다른 풀 이름' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['target']['nodes']['cmp']['sip'] = {}             # 역할과 다른 블록
        self.assertTrue(any('블록' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['volte_ue_a']['source'] = {'db': 'cmp', 'table': 'volte_subscriptions', 'count': 10}
        self.assertTrue(any('source.db' in e for e in validate('topology', d)[1]))
        d = self._doc(); d['pools']['volte_ue_a']['source'] = {'db': 'db', 'table': 'volte_subscriptions', 'count': 10}
        self.assertEqual(validate('topology', d)[1], [])

    def test_during(self):
        doc = {'id': 'T-DUR', 'roles': {'a': {'pool': 'p'}, 'b': {'pool': 'p'}},
               'flow': [{'step': 'invite', 'from': 'a', 'to': 'b'},
                        {'step': 'media_hold', 'seconds': 10, 'during': [{'at_s': 3, 'step': 'dtmf', 'from': 'a', 'payload': '12#'}]}]}
        m, errs = validate('scenario', doc)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[1].during[0].payload, '12#')
        doc['flow'][1]['during'][0]['at_s'] = 11
        self.assertTrue(any('넘는다' in e for e in validate('scenario', doc)[1]))
        doc['flow'][1]['during'][0]['at_s'] = 1; doc['flow'][1]['during'][0]['from'] = 'zz'
        self.assertTrue(any('zz' in e for e in validate('scenario', doc)[1]))
        doc['flow'][0]['during'] = [{'at_s': 1, 'step': 'hold', 'from': 'a'}]
        self.assertTrue(any('media_hold' in e for e in validate('scenario', doc)[1]))

    def test_media_plane(self):
        """invite.media.rtp · media_send/media_stop 자리 검증 · 샘플 라이브러리 (§4 미디어 평면)."""
        def doc(flow):
            return {'id': 'T-MEDIA', 'roles': {'a': {'pool': 'p'}, 'b': {'pool': 'p'}}, 'flow': flow}
        ok = doc([{'step': 'invite', 'from': 'a', 'to': 'b', 'media': {'audio': 'pcmu', 'rtp': 'explicit'}},
                  {'step': 'answer', 'who': ['b']},
                  {'step': 'media_send', 'who': ['a'], 'sample': 'ringback', 'loop': False, 'after_ms': 200},
                  {'step': 'media_hold', 'seconds': 5, 'during': [{'at_s': 2, 'step': 'media_stop', 'who': ['a']},
                                                                   {'at_s': 3, 'step': 'media_send', 'who': ['b'], 'sample': 'tone'}]},
                  {'step': 'bye', 'from': 'a'}])
        m, errs = validate('scenario', ok)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[0].media.rtp, 'explicit')
        self.assertEqual(m.sample_refs(), ['ringback', 'tone'])
        # SDP 가 오가기 전(answer/progress 전)·호 밖·rtp none 호에는 못 둔다
        early = doc([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'media_send', 'who': ['a']}])
        self.assertTrue(any('SDP' in e for e in validate('scenario', early)[1]))
        after_bye = doc([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'answer', 'who': ['b']}, {'step': 'bye', 'from': 'a'},
                         {'step': 'media_stop', 'who': ['a']}])
        self.assertTrue(any('SDP' in e for e in validate('scenario', after_bye)[1]))
        none = doc([{'step': 'invite', 'from': 'a', 'to': 'b', 'media': {'rtp': 'none'}}, {'step': 'answer', 'who': ['b']},
                    {'step': 'media_send', 'who': ['a']}])
        self.assertTrue(any('none' in e for e in validate('scenario', none)[1]))
        # 필드 자리 — sample/loop 은 media_send 만, media.rtp 는 invite 만, 모드 오타 거절
        bad = doc([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'answer', 'who': ['b'], 'sample': 'x'}])
        self.assertTrue(any('sample' in e for e in validate('scenario', bad)[1]))
        bad = doc([{'step': 'invite', 'from': 'a', 'to': 'b', 'media': {'rtp': 'manual'}}])
        self.assertTrue(validate('scenario', bad)[1])
        bad = doc([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'answer', 'who': ['b'], 'media': {'rtp': 'none'}}])
        self.assertTrue(any('invite' in e for e in validate('scenario', bad)[1]))
        # 샘플 라이브러리 — 코덱 키·경로
        from services.tester_models import TopologyMedia
        TopologyMedia.model_validate({'samples': {'rb': {'pcmu': 'rb.pcmu', 'amr-wb': 'synthetic'}}})
        for m_bad in ({'rb': {'g722': 'x'}}, {'rb': {'pcmu': '/etc/passwd'}}, {'rb': {'pcmu': '../x'}}, {'rb': {}}, {'r b': {'pcmu': 'x'}}):
            with self.assertRaises(Exception):
                TopologyMedia.model_validate({'samples': m_bad})

    def test_vocab_consistency(self):
        from services.tester_models import STEP_VOCAB, WORKER_STEPS, METRIC_NAMES, StepKind
        import typing
        kinds = set(typing.get_args(StepKind))
        self.assertEqual(set(STEP_VOCAB), kinds)
        self.assertTrue(WORKER_STEPS <= kinds)
        for k, v in STEP_VOCAB.items():
            for m in v['metrics']:
                self.assertIn(m, METRIC_NAMES, f'{k}: {m}')


class SchemaFilesInSync(unittest.TestCase):
    def test_schema_dir_matches_models(self):
        d = os.path.join(_TESTER, 'schema')
        for name in SCHEMAS:
            p = os.path.join(d, f'{name}.schema.json')
            self.assertTrue(os.path.isfile(p), f'{p} 없음 — bin/gen-schemas 실행')
            with open(p, 'r', encoding='utf-8') as f:
                on_disk = json.load(f)
            self.assertEqual(on_disk, schema_json(name), f'{name}: 모델과 스키마 파일 불일치 — bin/gen-schemas 재실행')
        extra = {os.path.basename(p)[:-len('.schema.json')] for p in glob.glob(os.path.join(d, '*.schema.json'))} - set(SCHEMAS)
        self.assertEqual(extra, set(), '모델 없는 스키마 파일')


if __name__ == '__main__':
    unittest.main()
