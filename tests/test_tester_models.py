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
from services.tester_models import METRIC_NAMES, RATIO_METRICS, STEP_VOCAB, SAMPLE_CODECS  # noqa: E402


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

    def test_sds_plane(self):
        # sds_send plane: media(MSRP media plane) — sds_send 에만 둔다
        doc = self._scn()
        doc['flow'].append({'step': 'sds_send', 'from': 'a', 'to': 'b', 'payload': 'x', 'plane': 'media'})
        _, errs = validate('scenario', doc)
        self.assertEqual(errs, [])
        doc = self._scn()
        doc['flow'].append({'step': 'sds_send', 'from': 'a', 'to': 'b', 'payload': 'x', 'plane': 'msrp'})
        _, errs = validate('scenario', doc)
        self.assertTrue(errs)
        doc = self._scn()
        doc['flow'].append({'step': 'sds_recv', 'who': ['b'], 'plane': 'media'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('plane' in e for e in errs))

    def test_fd_steps(self):
        # MCData FD — fd_send 는 from + payload(합성 크기 | 샘플 파일), fd_recv 는 who + payload download|signal. plane/disposition 은 여기 못 둔다
        for payload in ('256k', '65536', '2m', 'sample_voice.amrwb', 'files/photo.jpg'):
            doc = self._scn()
            doc['flow'].append({'step': 'fd_send', 'from': 'a', 'to': 'b', 'payload': payload})
            doc['flow'].append({'step': 'fd_recv', 'who': ['b']})
            self.assertEqual(validate('scenario', doc)[1], [], payload)
        for bad, word in ((
            {'step': 'fd_send', 'to': 'b', 'payload': '1k'}, 'from'),
            ({'step': 'fd_send', 'from': 'a', 'to': 'b'}, 'payload'),
            ({'step': 'fd_send', 'from': 'a', 'to': 'b', 'payload': '../etc/passwd'}, 'payload'),
            ({'step': 'fd_send', 'from': 'a', 'to': 'b', 'payload': '1k', 'plane': 'media'}, 'plane'),
            ({'step': 'fd_recv', 'who': ['b'], 'payload': 'upload'}, 'download'),
            ({'step': 'fd_recv'}, 'who'),
        ):
            doc = self._scn()
            doc['flow'].append(bad)
            errs = validate('scenario', doc)[1]
            self.assertTrue(any(word in e for e in errs), (bad, errs))
        doc = self._scn()
        doc['flow'].append({'step': 'fd_recv', 'who': ['b'], 'payload': 'signal'})
        self.assertEqual(validate('scenario', doc)[1], [])
        # to 없는 fd_send = 그룹 세션(multi 역할 허용)
        from services.tester_models import Scenario
        sc = Scenario.model_validate({'id': 'T-FD', 'roles': {'a': {'pool': 'p'}, 'm': {'pool': 'p', 'multi': True}},
                                      'flow': [{'step': 'fd_send', 'from': 'a', 'payload': '1k'}, {'step': 'fd_recv', 'who': ['m']}]})
        self.assertTrue(sc.is_group_session())
        self.assertEqual(sc.multi_roles(), ['m'])

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

    def test_invite_dial_form(self):
        # invite.dial — 착신 역할의 E.164 를 국내형/국제 접두 꼴로 다이얼(대상 다이얼 플랜 번역 시험). invite 에만, 역할 대상에만
        doc = self._scn()
        doc['flow'][1] = {'step': 'invite', 'from': 'a', 'to': 'b', 'dial': 'national'}
        m, errs = validate('scenario', doc)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[1].dial, 'national')
        doc = self._scn()
        doc['flow'][1] = {'step': 'invite', 'from': 'a', 'to': '+82210001000', 'dial': 'national'}
        _, errs = validate('scenario', doc)
        self.assertTrue(any('dial' in e for e in errs), errs)
        doc = self._scn()
        doc['flow'].append({'step': 'bye', 'from': 'a', 'dial': 'national'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('dial' in e for e in errs), errs)
        doc = self._scn()
        doc['flow'][1] = {'step': 'invite', 'from': 'a', 'to': 'b', 'dial': 'local'}
        _, errs = validate('scenario', doc)
        self.assertTrue(errs)

    def test_topology_dial_plan(self):
        # SIP 노드 sip.dial_plan → 워커 계약 target_csp.dial_plan · 피어 풀 dial_plan 은 시드 Route 의 인바운드 플랜(test_tester_target)
        from services.tester_models import Topology
        with open(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml'), encoding='utf-8') as f:
            t = Topology.model_validate(yaml.safe_load(f))
        tc = t.target_csp_for('volte_ue_a')
        self.assertIsNotNone(tc.dial_plan)
        self.assertEqual((tc.dial_plan.country_code, tc.dial_plan.national_prefix, tc.dial_plan.international_prefix), ('82', '0', '00'))
        self.assertEqual(t.pools['pbx_hq'].dial_plan.country_code, '82')
        bad = yaml.safe_load(open(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml'), encoding='utf-8'))
        bad['target']['nodes']['csp']['sip']['dial_plan'] = {'country_code': '+82'}
        with self.assertRaises(Exception):
            Topology.model_validate(bad)

    def test_transfer_join_steps(self):
        # pickup 은 from + payload(피처코드|${var}) · replaces/join 은 from·to + 앞선 subscribe(dialog) · subscribe payload 는 이벤트 토큰 ·
        # publish payload 는 affiliate|deaffiliate, group 은 publish 에도 · 그룹 세션에 1:1 전달 단계 금지
        def bad(flow, key):
            doc = self._scn()
            doc['roles']['c'] = {'pool': 'volte_ue'}
            doc['flow'] = doc['flow'][:1] + flow
            _, errs = validate('scenario', doc)
            self.assertTrue(any(key in e for e in errs), (key, errs))
        bad([{'step': 'pickup', 'from': 'c'}], 'payload')
        bad([{'step': 'pickup', 'from': 'c', 'payload': 'xyz'}], '피처코드')
        bad([{'step': 'replaces', 'from': 'c'}], 'to')
        bad([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'replaces', 'from': 'c', 'to': 'b'}], 'subscribe')
        bad([{'step': 'subscribe', 'who': ['c'], 'to': 'a'}, {'step': 'invite', 'from': 'a', 'to': 'b'},
             {'step': 'join', 'from': 'c', 'to': 'b'}], 'subscribe')          # a 를 감시했는데 b 에 합류
        bad([{'step': 'subscribe', 'who': ['c'], 'payload': 'bad token!'}], '이벤트 패키지')
        bad([{'step': 'publish', 'who': ['c'], 'payload': 'join'}], 'affiliate')
        bad([{'step': 'invite', 'from': 'a', 'to': 'b', 'group': 'g1'}], 'group')
        doc = self._scn()
        doc['roles']['c'] = {'pool': 'volte_ue'}
        doc['flow'] = [doc['flow'][0],
                       {'step': 'subscribe', 'who': ['c'], 'to': 'b', 'expect': {'code': 200}},
                       {'step': 'invite', 'from': 'a', 'to': 'b'},
                       {'step': 'replaces', 'from': 'c', 'to': 'b', 'expect': {'code': 200}},
                       {'step': 'pickup', 'from': 'c', 'to': 'b', 'payload': '${pickup_code}'},
                       {'step': 'publish', 'who': ['c'], 'payload': 'deaffiliate', 'group': 'g1'},
                       {'step': 'bye', 'from': 'a'}]
        m, errs = validate('scenario', doc)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[3].to, 'b')
        from services.tester_models import WORKER_STEPS
        for k in ('pickup', 'subscribe', 'replaces', 'join', 'publish', 'refer'):
            self.assertIn(k, WORKER_STEPS)

    def test_ratio_metrics_are_metric_names(self):
        from services.tester_models import METRIC_NAMES, RATIO_METRICS, LOWER_BETTER_RATIOS
        for k in RATIO_METRICS:
            self.assertIn(k, METRIC_NAMES)
        # RFC 6076 SEER/ISA 는 카운터 비율(분모 invite_tx), ISA 는 낮을수록 좋다(기대치 상한)
        self.assertEqual(RATIO_METRICS['seer_pct'], ('seer_ok', 'invite_tx'))
        self.assertEqual(RATIO_METRICS['isa_pct'], ('isa_fail', 'invite_tx'))
        self.assertTrue(LOWER_BETTER_RATIOS <= set(RATIO_METRICS))
        # mos 는 타이머 지표 — 기대치 min(하한)으로 쓴다
        doc = self._scn()
        doc['flow'].insert(2, {'step': 'media_hold', 'seconds': 3, 'expect': {'mos': {'min': 3.5}, 'isa_pct': {'max': 1}}})
        _, errs = validate('scenario', doc)
        self.assertEqual(errs, [])

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
        TopologyMedia.model_validate({'samples': {'wb': {'g722': 'wb.g722'}}})   # G.722 raw 160 B/20 ms 도 샘플 코덱
        for m_bad in ({'rb': {'gsm': 'x'}}, {'rb': {'pcmu': '/etc/passwd'}}, {'rb': {'pcmu': '../x'}}, {'rb': {}}, {'r b': {'pcmu': 'x'}}):
            with self.assertRaises(Exception):
                TopologyMedia.model_validate({'samples': m_bad})

    def test_peer_fault_drop_tls_thig_dtmf(self):
        """피어 오류 주입 후속·TLS 상호인증·THIG·DTMF 방식(§3.1·§3.2) — 옛 bool dtmf 승계, drop 은 UDP 만, thig 는 ibcf 만, tls_client_auth 는 bind tls 만."""
        from services.tester_models import PeerPool, UePool, PoolCreate
        base = {'kind': 'peer', 'worker': 'w', 'peering': 'csp', 'profile': 'ibcf', 'bind': {'port': 5080}, 'domain': 'x.test',
                'identities': {'e164_range': ['+821', '+829']}}
        p = PeerPool.model_validate({**base, 'fault': {'drop_invite': 1, 'drop_pct': 10}, 'thig': True, 'dtmf': False, 'tls_verify': True})
        self.assertEqual((p.fault.drop_invite, p.fault.drop_pct, p.thig, p.dtmf, p.answer), (1, 10, True, 'off', 'normal'))
        self.assertEqual(PeerPool.model_validate({**base, 'dtmf': True}).dtmf, 'rfc4733')
        self.assertEqual(PeerPool.model_validate({**base, 'dtmf': 'inband'}).dtmf, 'inband')
        for bad in ({**base, 'bind': {'port': 5061, 'protocol': 'tls'}, 'fault': {'drop_invite': 1}},   # 재전송은 UDP 만
                    {**base, 'profile': 'pbx', 'identities': {'did_range': ['1000', '1009']}, 'thig': True},   # THIG 는 ibcf
                    {**base, 'tls_client_auth': True},                                                # bind udp 에 클라이언트 인증 요구
                    {**base, 'dtmf': 'tones'}, {**base, 'fault': {'drop_invite': 9}}):
            with self.assertRaises(Exception):
                PeerPool.model_validate(bad)
        PeerPool.model_validate({**base, 'bind': {'port': 5061, 'protocol': 'tls'}, 'tls_client_auth': True, 'tls_verify': True, 'tls_client_cert': True})
        u = UePool.model_validate({'kind': 'ue', 'worker': 'w', 'access': 'csp', 'source': {'creds': 'c.jsonl'}, 'transport': 'tls',
                                   'dtmf': 'inband', 'tls_verify': True, 'tls_client_cert': True})
        self.assertEqual((u.dtmf, u.tls_verify, u.tls_client_cert), ('inband', True, True))
        self.assertEqual(UePool.model_validate({'kind': 'ue', 'worker': 'w', 'access': 'csp', 'source': {'creds': 'c.jsonl'}, 'dtmf': False}).dtmf, 'off')
        pc = PoolCreate.model_validate({'pool': 'p', 'kind': 'ue', 'dtmf': 'inband', 'tls_verify': True, 'tls_client_cert': True,
                                        'target_csp': {'ip': '10.0.0.1', 'domain_volte': 'volte.test'}})
        self.assertEqual(pc.dtmf, 'inband')
        for k in ('retrans_rx_pct', 'thig_pct'):
            self.assertIn(k, METRIC_NAMES)
            self.assertIn(k, RATIO_METRICS)
        self.assertIsNone(STEP_VOCAB['progress']['kind'])   # 착신 UE 도 183 을 낸다
        self.assertIn('g722', SAMPLE_CODECS)

    def test_check_step_and_subscribe_literal(self):
        """check 단계(관측 정합 판정) — payload 는 CHECK_KINDS 또는 ${var}, 로스터 판정은 to 필수, who 필수 · subscribe.to 는 대표번호 리터럴도 된다(F7)."""
        from services.tester_models import CHECK_KINDS
        def doc(flow, roles=None):
            return {'id': 'T-CHK', 'roles': roles or {'a': {'pool': 'p'}, 'b': {'pool': 'p'}}, 'flow': flow}
        ok = doc([{'step': 'register', 'who': ['a', 'b']}, {'step': 'subscribe', 'who': ['a'], 'to': '${pilot}'}, {'step': 'subscribe', 'who': ['a'], 'to': 'b'},
                  {'step': 'invite', 'from': 'b', 'to': '${pilot}'}, {'step': 'bye', 'from': 'b'},
                  {'step': 'check', 'who': ['a'], 'payload': 'dialog_consistent', 'after_ms': 1000, 'expect': {'check_pct': 100}},
                  {'step': 'check', 'who': ['a'], 'payload': '${roster}', 'to': 'b'}])
        m, errs = validate('scenario', ok)
        self.assertEqual(errs, [])
        self.assertEqual(m.flow[1].to, '${pilot}')
        for bad, word in ((doc([{'step': 'check', 'who': ['a'], 'payload': 'nope'}]), 'payload'),
                          (doc([{'step': 'check', 'who': ['a'], 'payload': 'conference_roster_visible'}]), 'to'),
                          (doc([{'step': 'check', 'payload': 'dialog_consistent'}]), 'who'),
                          (doc([{'step': 'invite', 'from': 'a', 'to': 'b'}, {'step': 'subscribe', 'who': ['a'], 'to': 'zzz'}]), '정의되지')):
            self.assertTrue(any(word in e for e in validate('scenario', bad)[1]), (word, validate('scenario', bad)[1]))
        self.assertIn('check', STEP_VOCAB)
        self.assertIn('check_pct', RATIO_METRICS)
        self.assertEqual(len(CHECK_KINDS), 4)

    def test_ptt_group_session_rules(self):
        """그룹 세션(group_call) 시나리오 규칙 — multi 역할 하나·같은 풀·group_call.from 단일/to multi·floor 는 group_call 뒤·1:1 단계 금지."""
        def doc(flow, roles=None):
            return {'id': 'T-PTT', 'roles': roles or {'t': {'pool': 'p'}, 'l': {'pool': 'p', 'multi': True}}, 'flow': flow}
        ok = doc([{'step': 'register', 'who': ['t', 'l']}, {'step': 'group_call', 'from': 't', 'to': 'l', 'media': {'rtp': 'none'}},
                  {'step': 'floor_request', 'who': ['t'], 'payload': 'granted', 'expect': {'floor_grant_ms': {'p95': 300}, 'floor_grant_pct': {'min': 100}}},
                  {'step': 'media_hold', 'seconds': 2}, {'step': 'floor_release', 'who': ['t'], 'expect': {'floor_idle_ms': {'p95': 500}}},
                  {'step': 'bye', 'who': ['t', 'l']}])
        m, errs = validate('scenario', ok)
        self.assertEqual(errs, [])
        self.assertTrue(m.is_group_session())
        self.assertEqual(m.multi_roles(), ['l'])
        for bad, word in (
            (doc([{'step': 'group_call', 'from': 'l'}]), '단일 역할'),
            (doc([{'step': 'group_call', 'from': 't', 'to': 't'}]), 'multi 역할'),
            (doc([{'step': 'floor_request', 'who': ['t']}, {'step': 'group_call', 'from': 't'}]), 'group_call 뒤'),
            (doc([{'step': 'group_call', 'from': 't'}, {'step': 'floor_request', 'who': ['t'], 'payload': 'maybe'}]), 'payload'),
            (doc([{'step': 'group_call', 'from': 't'}, {'step': 'answer', 'who': ['t']}]), '1:1'),
            (doc([{'step': 'group_call', 'from': 't'}], {'t': {'pool': 'p'}, 'l': {'pool': 'q', 'multi': True}}), '같은 풀'),
            (doc([{'step': 'group_call', 'from': 't'}], {'t': {'pool': 'p'}, 'l': {'pool': 'p', 'multi': True}, 'k': {'pool': 'p', 'multi': True}}), '하나만'),
            (doc([{'step': 'invite', 'from': 't', 'to': 'l'}]), 'group_call(또는 그룹 SDS/FD sds_send·fd_send)이 있는'),
            (doc([{'step': 'group_call', 'to': 'l'}]), 'from'),
            (doc([{'step': 'group_call', 'from': 't'}, {'step': 'bye', 'from': 't', 'group': 'g'}]), 'group 은'),
        ):
            self.assertTrue(any(word in e for e in validate('scenario', bad)[1]), (word, validate('scenario', bad)[1]))
        # 풀 service · source.ptt_group
        from services.tester_models import UePool
        p = UePool.model_validate({'kind': 'ue', 'worker': 'w', 'access': 'csp', 'service': 'ptt',
                                   'source': {'db': 'db', 'table': 'ptt_subscriptions', 'count': 5, 'ptt_group': 'g001'}})
        self.assertEqual(p.service, 'ptt')
        with self.assertRaises(Exception):
            UePool.model_validate({'kind': 'ue', 'worker': 'w', 'access': 'csp',
                                   'source': {'db': 'db', 'table': 'volte_subscriptions', 'count': 5, 'ptt_group': 'g001'}})

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
