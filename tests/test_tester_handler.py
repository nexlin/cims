"""계측기 API 핸들러 단위시험 (handlers.tester 직접 호출 — 서버 미기동).

Covers:
  - RBAC: 토큰 없음 401, 조회 monitor / 토폴로지 쓰기 operator / 삭제 manager
  - /health · /schema · /validate(doc, yaml) · /scenarios · /profiles
  - /topologies CRUD — 검증 실패 400·errors, 저장은 검증 통과분만
  - /runs POST 검증(400/404/403) · stop/rate/report 미존재 404 · /runs 빈 색인 · 색인 저장 후 조회 · 보존 스윕
  - /events = text/event-stream StreamingResponse
  - E 단계: 시나리오/프로파일 PUT·DELETE(운영자본만, id 일치), /topologies/<id>/check, /runs/<id> DELETE·/series,
    /runs/compare(기준 대비 delta·회귀), 모듈 /api/v1/api-docs 자기기술

각 테스트는 tmpdir 로 CimsRuntimeDir·Tester.DataDir 격리. 토큰은 admin_auth 로 직접 발급.
"""
import asyncio
import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

from httpsrv.handler import HandlerArgs  # noqa: E402

# 핸들러·스토어는 setUpModule 에서 import 한다 — 같은 프로세스에서 뒤에 import 되는 다른 시험 파일이
# services/handlers 모듈 캐시를 비우면(위 관용) 여기서 미리 잡아 둔 참조가 옛 모듈 객체(리스 미획득
# file_store)를 가리켜 `assert_writable` 에서 깨진다. 전 시험 모듈 import 가 끝난 뒤 잡으면 한 인스턴스다.
admin_auth = H = S = RunRecord = None

_SECRET = 'unit-test-secret'


def _token(role):
    import jwt, time  # vendor
    return jwt.encode({'sub': 'u', 'login_id': 'u', 'role': role, 'exp': int(time.time()) + 600},
                      _SECRET, algorithm='HS256')


def _call(method, path, role='monitor', body=None, query=None):
    # controller 는 헤더 키를 소문자로 넘긴다(admin_auth.extract_admin_jwt 규약).
    headers = {'authorization': f'Bearer {_token(role)}'} if role else {}
    args = HandlerArgs(method=method, full_path=path, client_ip='127.0.0.1', client_port=1,
                       query_params=query or {}, headers=headers, body=body)
    return asyncio.run(H.handle_tester(args, {'config': _CFG}))


_TMP = None
_CFG = None


def setUpModule():
    global _TMP, _CFG, admin_auth, H, S, RunRecord
    from services import admin_auth as _aa
    from handlers import tester as _H
    from services import tester_store as _S
    from services.tester_models import RunRecord as _RR
    admin_auth, H, S, RunRecord = _aa, _H, _S, _RR
    _TMP = tempfile.mkdtemp(prefix='tester-ut-')
    _CFG = {'CimsRuntimeDir': os.path.join(_TMP, 'runtime'),
            'CimsAuth': {'JwtSecret': _SECRET},
            'Tester': {'DataDir': os.path.join(_TMP, 'data'), 'RunRetainDays': 30}}
    admin_auth.init(_CFG)
    from services import file_store, lease
    lease.acquire(file_store.runtime_root(_CFG))   # 관리 store 는 단일 writer(리스 펜싱)
    H.init(_TESTER, _CFG)


def tearDownModule():
    shutil.rmtree(_TMP, ignore_errors=True)


class Rbac(unittest.TestCase):
    def test_no_token_401(self):
        self.assertEqual(_call('GET', '/api/v1/tester/health', role=None).status, 401)

    def test_monitor_can_read(self):
        self.assertEqual(_call('GET', '/api/v1/tester/health', role='monitor').status, 200)

    def test_monitor_cannot_write(self):
        r = _call('POST', '/api/v1/tester/topologies', role='monitor', body={'name': 'x'})
        self.assertEqual(r.status, 403)

    def test_operator_cannot_delete(self):
        r = _call('DELETE', '/api/v1/tester/topologies/1', role='operator')
        self.assertEqual(r.status, 403)


class ReadApis(unittest.TestCase):
    def test_health(self):
        r = _call('GET', '/api/v1/tester/health')
        self.assertEqual(r.body['module'], 'oam-cims-tester')
        self.assertGreaterEqual(r.body['scenarios'], 3)
        self.assertGreaterEqual(r.body['profiles'], 3)

    def test_schema(self):
        r = _call('GET', '/api/v1/tester/schema')
        self.assertIn('scenario', r.body['schemas'])
        r = _call('GET', '/api/v1/tester/schema/scenario')
        self.assertIn('properties', r.body)
        self.assertEqual(_call('GET', '/api/v1/tester/schema/nope').status, 404)

    def test_validate_doc_and_yaml(self):
        r = _call('POST', '/api/v1/tester/validate', role='operator',
                  body={'kind': 'profile', 'doc': {'model': 'constant', 'rate': 1, 'duration_s': 10}})
        self.assertTrue(r.body['ok'])
        r = _call('POST', '/api/v1/tester/validate', role='operator',
                  body={'kind': 'profile', 'yaml': 'model: step\nstart: 5\n'})
        self.assertFalse(r.body['ok'])
        self.assertTrue(r.body['errors'])
        r = _call('POST', '/api/v1/tester/validate', role='operator', body={'kind': 'profile', 'yaml': ':::'})
        self.assertFalse(r.body['ok'])

    def test_scenarios_and_profiles(self):
        r = _call('GET', '/api/v1/tester/scenarios')
        ids = {s['id'] for s in r.body['scenarios']}
        self.assertIn('VOLTE-CALL-BASIC', ids)
        self.assertTrue(all(s['errors'] == [] for s in r.body['scenarios']))
        r = _call('GET', '/api/v1/tester/scenarios/VOLTE-CALL-BASIC')
        self.assertTrue(r.body['valid'])
        self.assertEqual(_call('GET', '/api/v1/tester/scenarios/NOPE').status, 404)
        r = _call('GET', '/api/v1/tester/profiles')
        self.assertIn('step_5_to_100', {p['name'] for p in r.body['profiles']})

    def test_user_scenario_overrides_bundled_and_broken_is_listed(self):
        ud = S.user_scenarios_dir()
        os.makedirs(os.path.join(ud, 'volte'), exist_ok=True)
        broken = os.path.join(ud, 'volte', 'broken.yaml')
        with open(broken, 'w') as f:
            f.write('id: BROKEN-ONE\nroles: {a: {pool: p}}\nflow: [{step: answer}]\n')
        try:
            r = _call('GET', '/api/v1/tester/scenarios')
            row = next(s for s in r.body['scenarios'] if s['id'] == 'BROKEN-ONE')
            self.assertEqual(row['source'], 'user')
            self.assertTrue(row['errors'])
        finally:
            os.remove(broken)


class Topologies(unittest.TestCase):
    def _doc(self, name='t1'):
        return {'name': name, 'hosts': {'h1': {'ip': '10.0.0.1'}}, 'workers': [{'name': 'w1', 'host': 'h1'}],
                'target': {'name': 'sut', 'nodes': {'csp': {'role': 'sip', 'host': 'h1', 'sip': {'access': {'udp': 5060}}}}},
                'pools': {'ue': {'kind': 'ue', 'worker': 'w1', 'access': 'csp', 'source': {'creds': 'creds/x.jsonl'}}}}

    def test_v1_record_migrates_to_v2(self):
        # 이전 꼴(target.csp/csc/oam · workers[].url · bind.ip) 은 저장·읽기 때 호스트›워커·노드›풀 로 승계된다
        v1 = {'name': 'old', 'target': {'name': 'sut', 'csp': {'ip': '10.0.0.5', 'udp': 15060, 'tls': 15061, 'domain_volte': 'volte.test',
                                                              'peering': {'port': 15070}},
                                        'csc': {'host': '10.0.0.5', 'port': 4430}, 'oam': {'url': 'https://10.0.0.5:4419', 'token_env': 'T'}},
              'workers': [{'name': 'w1', 'url': 'http://10.0.0.61:7100', 'cpus': 4}, {'name': 'w2', 'url': 'http://10.0.0.62:7100'}],
              'pools': {'ue': {'kind': 'ue', 'source': {'creds': 'creds/x.jsonl'}, 'transport': 'tls'},
                        'kt': {'kind': 'peer', 'profile': 'ibcf', 'bind': {'ip': '10.0.0.62', 'port': 5080}, 'domain': 'kt.test',
                               'identities': {'e164_range': ['+821', '+829']}}}}
        r = _call('POST', '/api/v1/tester/topologies', role='operator', body=v1)
        self.assertEqual(r.status, 201, r.body)
        doc = r.body['doc']
        try:
            self.assertEqual(sorted(doc['hosts']), ['h10_0_0_5', 'h10_0_0_61', 'h10_0_0_62'])
            self.assertEqual(doc['target']['nodes']['csp']['sip']['listeners']['tls'], {'edge': 'access', 'port': 15061, 'protocol': 'tls'})
            self.assertEqual(doc['target']['nodes']['csp']['sip']['listeners']['peering']['port'], 15070)
            self.assertEqual(doc['target']['nodes']['csp']['sip']['domains'], ['volte.test'])
            self.assertEqual(doc['target']['nodes']['oam']['oam']['port'], 4419)
            self.assertEqual(doc['target']['nodes']['csc']['api']['port'], 4430)
            self.assertEqual([(w['name'], w['host'], w['port']) for w in doc['workers']], [('w1', 'h10_0_0_61', 7100), ('w2', 'h10_0_0_62', 7100)])
            self.assertEqual((doc['pools']['ue']['worker'], doc['pools']['ue']['access']), ('w1', 'csp'))
            self.assertEqual((doc['pools']['kt']['worker'], doc['pools']['kt']['peering']), ('w2', 'csp'))   # bind.ip → 그 주소의 워커
            self.assertNotIn('ip', doc['pools']['kt']['bind'])
        finally:
            _call('DELETE', f"/api/v1/tester/topologies/{r.body['id']}", role='manager')

    def test_crud(self):
        r = _call('POST', '/api/v1/tester/topologies', role='operator', body=self._doc())
        self.assertEqual(r.status, 201, r.body)
        tid = r.body['id']
        r = _call('GET', f'/api/v1/tester/topologies/{tid}')
        self.assertEqual(r.body['name'], 't1')
        r = _call('PUT', f'/api/v1/tester/topologies/{tid}', role='operator', body=self._doc('t2'))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body['name'], 't2')
        r = _call('GET', '/api/v1/tester/topologies')
        self.assertTrue(any(t['id'] == tid for t in r.body['topologies']))
        r = _call('DELETE', f'/api/v1/tester/topologies/{tid}', role='manager')
        self.assertEqual(r.status, 200)
        self.assertEqual(_call('GET', f'/api/v1/tester/topologies/{tid}').status, 404)

    def test_invalid_rejected_with_errors(self):
        bad = self._doc(); bad['pools']['ue']['transport'] = 'sctp'
        r = _call('POST', '/api/v1/tester/topologies', role='operator', body=bad)
        self.assertEqual(r.status, 400)
        self.assertTrue(any('transport' in e for e in r.body['errors']))
        self.assertEqual(_call('PUT', '/api/v1/tester/topologies/9999', role='operator', body=self._doc()).status, 404)


class Runs(unittest.TestCase):
    def test_post_validates_request(self):
        # RunRequest 검증 — topology 없음 400, 모르는 시나리오 404, 모르는 토폴로지 404 (워커에 닿기 전에 거절)
        r = _call('POST', '/api/v1/tester/runs', role='operator', body={'scenario_id': 'VOLTE-CALL-BASIC'})
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body['error'], 'invalid_run_request')
        r = _call('POST', '/api/v1/tester/runs', role='operator', body={'scenario_id': 'NOPE-1', 'topology': 'x'})
        self.assertEqual(r.status, 404)
        r = _call('POST', '/api/v1/tester/runs', role='operator', body={'scenario_id': 'VOLTE-CALL-BASIC', 'topology': 'nope'})
        self.assertEqual(r.status, 404)
        self.assertIn('topology_not_found', r.body['error'])
        # monitor 는 run 을 시작할 수 없다
        r = _call('POST', '/api/v1/tester/runs', role='monitor', body={'scenario_id': 'VOLTE-CALL-BASIC', 'topology': 'x'})
        self.assertEqual(r.status, 403)

    def test_stop_rate_report_unknown_run(self):
        self.assertEqual(_call('POST', '/api/v1/tester/runs/nope/stop', role='operator').status, 404)
        self.assertEqual(_call('POST', '/api/v1/tester/runs/nope/rate', role='operator', body={'rate_saps': 1}).status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/nope/report').status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/nope/events').body, {'events': []})

    def test_index_roundtrip_and_purge(self):
        self.assertEqual(_call('GET', '/api/v1/tester/runs').body['runs'], [])
        S.save_run_index(RunRecord(id='r-2020', scenario_id='X-Y', topology='t',
                                   started_at='2020-01-01T00:00:00', ended_at='2020-01-01T00:10:00', verdict='pass'))
        S.save_run_index(RunRecord(id='r-now', scenario_id='X-Y', topology='t',
                                   started_at='2999-01-01T00:00:00', verdict='running'))
        r = _call('GET', '/api/v1/tester/runs')
        self.assertEqual([x['id'] for x in r.body['runs']], ['r-now', 'r-2020'])
        self.assertEqual(_call('GET', '/api/v1/tester/runs/r-2020').body['verdict'], 'pass')
        self.assertEqual(S.purge_runs(0), 0)            # 무제한
        self.assertEqual(S.purge_runs(30), 1)           # 오래된 pass 만, running 은 보존
        self.assertEqual(_call('GET', '/api/v1/tester/runs/r-2020').status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/r-now').status, 200)


class ScenarioProfileWrite(unittest.TestCase):
    _SC = ('id: UT-EDIT-ONE\ntitle: 편집 시험\ntags: [volte]\nroles: {a: {pool: p}}\n'
           'flow: [{step: register, who: [a]}]\n')

    def test_scenario_put_get_delete(self):
        r = _call('PUT', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='operator', body={'yaml': self._SC})
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(r.body['source'], 'user')
        self.assertEqual(r.body['steps'], 1)
        r = _call('GET', '/api/v1/tester/scenarios/UT-EDIT-ONE')
        self.assertEqual(r.status, 200)
        self.assertIn('title: 편집 시험', r.body['yaml'])
        self.assertEqual(r.body['source'], 'user')
        self.assertTrue(r.body['valid'])
        # id 불일치·검증 실패는 저장하지 않는다
        r = _call('PUT', '/api/v1/tester/scenarios/UT-EDIT-TWO', role='operator', body={'yaml': self._SC})
        self.assertEqual(r.status, 400)
        self.assertTrue(any('id 불일치' in e for e in r.body['errors']))
        r = _call('PUT', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='operator', body={'yaml': 'id: UT-EDIT-ONE\nflow: []\n'})
        self.assertEqual(r.status, 400)
        self.assertEqual(_call('PUT', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='operator', body={'yaml': ': ['}).status, 400)
        self.assertEqual(_call('PUT', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='monitor', body={'yaml': self._SC}).status, 403)
        # 삭제 — operator 403, manager 200, 동봉본 409
        self.assertEqual(_call('DELETE', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='operator').status, 403)
        self.assertEqual(_call('DELETE', '/api/v1/tester/scenarios/UT-EDIT-ONE', role='manager').status, 200)
        self.assertEqual(_call('GET', '/api/v1/tester/scenarios/UT-EDIT-ONE').status, 404)
        r = _call('DELETE', '/api/v1/tester/scenarios/VOLTE-CALL-BASIC', role='manager')
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body['error'], 'bundled_read_only')
        self.assertEqual(_call('DELETE', '/api/v1/tester/scenarios/NOPE-X', role='manager').status, 404)

    def test_bundled_override_and_profile(self):
        # 동봉 id 로 저장하면 운영자본이 이긴다(override) — 지우면 동봉본으로 돌아간다
        r = _call('GET', '/api/v1/tester/scenarios/VOLTE-REGISTER')
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body['source'], 'bundled')
        text = r.body['yaml'].replace('title: ', 'title: override ')
        r = _call('PUT', '/api/v1/tester/scenarios/VOLTE-REGISTER', role='operator', body={'yaml': text})
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(r.body['source'], 'user')
        self.assertEqual(_call('DELETE', '/api/v1/tester/scenarios/VOLTE-REGISTER', role='manager').status, 200)
        self.assertEqual(_call('GET', '/api/v1/tester/scenarios/VOLTE-REGISTER').body['source'], 'bundled')
        # 프로파일 — name 은 경로와 일치(문서 name 생략 가능)
        r = _call('PUT', '/api/v1/tester/profiles/ut_const', role='operator', body={'yaml': 'model: constant\nrate: 2\nduration_s: 10\n'})
        self.assertEqual(r.status, 200, r.body)
        self.assertEqual(r.body['model'], 'constant')
        r = _call('PUT', '/api/v1/tester/profiles/ut_const', role='operator', body={'yaml': 'name: other\nmodel: constant\nrate: 2\n'})
        self.assertEqual(r.status, 400)
        r = _call('GET', '/api/v1/tester/profiles/ut_const')
        self.assertIn('rate: 2', r.body['yaml'])
        self.assertEqual(_call('DELETE', '/api/v1/tester/profiles/ut_const', role='manager').status, 200)
        self.assertEqual(_call('DELETE', '/api/v1/tester/profiles/step_5_to_100', role='manager').status, 409)


class TopologyCheck(unittest.TestCase):
    def test_check_unreachable_target(self):
        # 127.0.0.1 의 닫힌 포트 — 항목마다 ok=False 와 이유가 남고 200 으로 돌아온다(검사 실패 ≠ API 실패)
        doc = {'name': 'chk', 'hosts': {'h1': {'ip': '127.0.0.1'}}, 'workers': [{'name': 'w1', 'host': 'h1', 'port': 1}],
               'target': {'name': 'sut', 'nodes': {'csp': {'role': 'sip', 'host': 'h1', 'sip': {'access': {'udp': 1, 'tcp': 1, 'tls': 1}}}}},
               'pools': {'ue': {'kind': 'ue', 'worker': 'w1', 'access': 'csp', 'source': {'creds': 'creds/x.jsonl'}}}}
        r = _call('POST', '/api/v1/tester/topologies', role='operator', body=doc)
        tid = r.body['id']
        try:
            r = _call('POST', f'/api/v1/tester/topologies/{tid}/check', role='operator')
            self.assertEqual(r.status, 200, r.body)
            self.assertFalse(r.body['ok'])
            names = {i['name']: i for i in r.body['items']}
            for n in ('csp:udp', 'csp:tcp', 'csp:tls', 'worker_w1'):
                self.assertIn(n, names)
            self.assertFalse(names['csp:tcp']['ok'])
            self.assertFalse(names['worker_w1']['ok'])
            self.assertEqual(names['csp:tcp']['target'], {'kind': 'node', 'id': 'csp'})
            self.assertEqual(names['worker_w1']['target'], {'kind': 'worker', 'id': 'w1'})
            self.assertEqual(_call('POST', f'/api/v1/tester/topologies/{tid}/check', role='monitor').status, 403)
            self.assertEqual(_call('POST', '/api/v1/tester/topologies/9999/check', role='operator').status, 404)
        finally:
            _call('DELETE', f'/api/v1/tester/topologies/{tid}', role='manager')


class PlanAndVocab(unittest.TestCase):
    def test_vocab(self):
        r = _call('GET', '/api/v1/tester/scenarios/vocab')
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body['steps']['register']['supported'])
        self.assertTrue(r.body['steps']['group_call']['supported'])
        self.assertEqual(r.body['steps']['floor_request']['kind'], 'ptt')
        self.assertFalse(r.body['steps']['sds_send']['supported'])
        self.assertEqual(r.body['steps']['progress']['kind'], 'peer')
        self.assertIn('srd_ms', r.body['metrics'])
        self.assertEqual(r.body['q850']['16'], '정상 종료')

    def test_compile_check_and_plan(self):
        os.makedirs(os.path.join(S.user_scenarios_dir(), 'creds'), exist_ok=True)
        with open(os.path.join(S.user_scenarios_dir(), 'creds', 'plan.jsonl'), 'w') as f:
            for i in range(6):
                f.write(json.dumps({'user': f'+8210000000{i:02d}', 'ha1': 'ab' * 16}) + '\n')
        doc = {'name': 'plan-t', 'hosts': {'h1': {'ip': '10.0.0.1'}, 'hw': {'ip': '127.0.0.1'}},
               'workers': [{'name': 'w1', 'host': 'hw', 'port': 1, 'cpus': 2}],
               'target': {'name': 'sut', 'nodes': {'csp': {'role': 'sip', 'host': 'h1', 'sip': {'access': {'udp': 5060, 'domains': ['volte.test']}}}}},
               'pools': {'volte_ue': {'kind': 'ue', 'worker': 'w1', 'access': 'csp', 'source': {'creds': 'creds/plan.jsonl'}}}}
        tid = _call('POST', '/api/v1/tester/topologies', role='operator', body=doc).body['id']
        try:
            body = {'scenario_id': 'VOLTE-CALL-BASIC', 'topology_id': tid, 'profile': 'step_5_to_100', 'probe': False}
            r = _call('POST', '/api/v1/tester/scenarios/compile-check', role='operator', body=body)
            self.assertEqual(r.status, 200, r.body)
            self.assertTrue(r.body['ok'], r.body['errors'])
            self.assertEqual(r.body['roles']['caller']['workers'], {'w1': ['volte_ue', 0, 3]})
            self.assertEqual(r.body['roles']['callee']['workers'], {'w1': ['volte_ue', 3, 6]})
            self.assertEqual(r.body['phases'], {'prelude': [0], 'body': [1, 2, 3, 4], 'epilogue': []})
            self.assertEqual([p['step'] for p in r.body['procedure']], ['register', 'invite', 'answer', 'media_hold', 'bye'])
            self.assertEqual(r.body['bindings'], {'ht': 20})
            self.assertEqual(r.body['little']['sdt_s'], 22.5)             # 1 + 1.5 + 20
            self.assertEqual(r.body['little']['first_short_rate'], 5)     # 5 × 22.5 = 113 > 3 신원
            self.assertTrue(any('Little' in w for w in r.body['warnings']))
            self.assertEqual(r.body['estimate']['duration_s'], 20 * 300 + 22 + 5)
            self.assertTrue(r.body['workers'][0]['in_run'])
            self.assertEqual(r.body['workers'][0]['rate_saps'], 5.0)
            # 같은 함수 — runs/plan (단발) · 미저장 편집본(yaml) · 오류는 errors 로
            r2 = _call('POST', '/api/v1/tester/runs/plan', role='operator', body={'scenario_id': 'VOLTE-CALL-BASIC', 'topology': 'plan-t', 'instances': 2, 'bindings': {'ht': 3}, 'probe': False})
            self.assertTrue(r2.body['ok'], r2.body)
            self.assertEqual(r2.body['max_instances'], 2)
            self.assertEqual(r2.body['estimate']['model'], 'single')
            y = 'id: UT-PLAN\nroles: { a: { pool: volte_ue }, b: { pool: volte_ue } }\nflow:\n  - { step: invite, from: a, to: b }\n  - { step: progress, who: [b] }\n'
            r3 = _call('POST', '/api/v1/tester/scenarios/compile-check', role='operator', body={'yaml': y, 'topology_id': tid, 'probe': False})
            self.assertEqual(r3.status, 200)
            self.assertFalse(r3.body['ok'])
            self.assertTrue(any('피어 풀' in e for e in r3.body['errors']), r3.body['errors'])
            self.assertEqual(_call('POST', '/api/v1/tester/runs/plan', role='monitor', body=body).status, 403)
            self.assertEqual(_call('POST', '/api/v1/tester/runs/plan', role='operator', body={'scenario_id': 'NOPE', 'topology_id': tid}).status, 404)
        finally:
            _call('DELETE', f'/api/v1/tester/topologies/{tid}', role='manager')

    def test_hold_hist_sip_not_found(self):
        self.assertEqual(_call('POST', '/api/v1/tester/runs/nope/hold', role='operator', body={'hold': True}).status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/nope/hist', query={'timer': 'srd_ms'}).status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/nope/sip/abc').status, 404)


class RunResults(unittest.TestCase):
    def test_index_filters(self):
        S.save_run_index(RunRecord(id='f-1', scenario_id='X-A', topology='t', profile='p1', started_at='2020-01-01T00:00:00', verdict='pass',
                                   target_build='csp 1 (dep 1)', label='night'))
        S.save_run_index(RunRecord(id='f-2', scenario_id='X-B', topology='t', started_at='2020-02-01T00:00:00', verdict='fail',
                                   target_build='csp 2 (dep 1)'))
        try:
            ids = lambda q: [r['id'] for r in _call('GET', '/api/v1/tester/runs', query=q).body['runs']]
            self.assertEqual(ids({'scenario': 'X-A'}), ['f-1'])
            self.assertEqual(ids({'verdict': 'fail,error'}), ['f-2'])
            self.assertEqual(ids({'build': 'csp 2 (dep 1)'}), ['f-2'])
            self.assertEqual(ids({'since': '2020-01-15'}), ['f-2'])
            self.assertEqual(ids({'label': 'NIGHT'}), ['f-1'])
            self.assertEqual(ids({'load': '1'}), ['f-1'])
            self.assertEqual(set(ids({})) >= {'f-1', 'f-2'}, True)
        finally:
            _call('DELETE', '/api/v1/tester/runs/f-1', role='manager')
            _call('DELETE', '/api/v1/tester/runs/f-2', role='manager')

    def test_delete_series_compare(self):
        self.assertEqual(_call('DELETE', '/api/v1/tester/runs/nope', role='manager').status, 404)
        self.assertEqual(_call('DELETE', '/api/v1/tester/runs/nope', role='operator').status, 403)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/nope/series').status, 404)
        self.assertEqual(_call('GET', '/api/v1/tester/runs/compare', query={'ids': 'a'}).status, 400)
        S.save_run_index(RunRecord(id='c-base', scenario_id='X-Y', topology='t', started_at='2020-01-01T00:00:00',
                                   ended_at='2020-01-01T00:10:00', verdict='pass', target_build='csp 1 (dep 1)',
                                   summary={'ser_pct': 100.0, 'srd_ms_p95': 100.0, 'rtp_loss_pct': 0.0, 'attempts': 10}))
        S.save_run_index(RunRecord(id='c-new', scenario_id='X-Y', topology='t', started_at='2020-01-02T00:00:00',
                                   ended_at='2020-01-02T00:10:00', verdict='pass', target_build='csp 2 (dep 1)',
                                   summary={'ser_pct': 99.0, 'srd_ms_p95': 120.0, 'rtp_loss_pct': 0.1, 'attempts': 10}))
        try:
            r = _call('GET', '/api/v1/tester/runs/c-base/series')
            self.assertEqual(r.status, 200)
            self.assertEqual(r.body['t'], [])          # metrics.sqlite 없음 = 빈 시계열
            r = _call('GET', '/api/v1/tester/runs/compare', query={'ids': 'c-base,c-new,missing'})
            self.assertEqual(r.status, 200, r.body)
            self.assertEqual(r.body['baseline'], 'c-base')
            self.assertTrue(r.body['runs'][2]['missing'])
            m = {x['metric']: x for x in r.body['metrics']}
            self.assertTrue(m['ser_pct']['regression'][1])       # 100 → 99 (0.5 pt 초과) 회귀
            self.assertTrue(m['srd_ms_p95']['regression'][1])    # 100 → 120 (5 % 초과) 회귀
            self.assertFalse(m['rtp_loss_pct']['regression'][1]) # 0 → 0.1 (0.5 pt 이내)
            self.assertIsNone(m['ser_pct']['regression'][2])     # 없는 run
            self.assertEqual(r.body['regressions'], 2)
            self.assertTrue(r.body['same_scenario'])
            r = _call('GET', '/api/v1/tester/runs/compare', query={'ids': 'c-base,c-new', 'format': 'md'})
            self.assertIsNotNone(r.response)
            self.assertIn(b'| ser_pct |', r.response.body)
            r = _call('GET', '/api/v1/tester/runs/compare', query={'ids': 'c-base,c-new', 'format': 'csv'})
            self.assertTrue(r.response.body.startswith(b'metric,direction,c-base,c-new,delta_c-new'))
            self.assertEqual(_call('DELETE', '/api/v1/tester/runs/c-new', role='manager').status, 200)
            self.assertEqual(_call('GET', '/api/v1/tester/runs/c-new').status, 404)
        finally:
            _call('DELETE', '/api/v1/tester/runs/c-base', role='manager')
            _call('DELETE', '/api/v1/tester/runs/c-new', role='manager')


class ApiDocs(unittest.TestCase):
    def test_module_self_description(self):
        args = HandlerArgs(method='GET', full_path='/api/v1/api-docs', client_ip='127.0.0.1', client_port=1,
                           query_params={}, headers={'authorization': f'Bearer {_token("monitor")}'})
        r = asyncio.run(H.handle_api_docs(args, {'config': _CFG}))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body['modules'], ['oam-cims-tester'])
        ids = {a['id'] for a in r.body['apis']}
        for need in ('tester.health', 'tester.runs', 'tester.events', 'tester.scenarios', 'tester.profiles',
                     'tester.topologies', 'tester.workers', 'tester.run.report', 'tester.run.series', 'tester.runs.compare',
                     'tester.scenarios.vocab', 'tester.scenarios.compile_check', 'tester.runs.plan', 'tester.run.hold',
                     'tester.run.hist', 'tester.run.sip', 'tester.run.target_alerts'):
            self.assertIn(need, ids)
        self.assertTrue(all(a['module'] == 'oam-cims-tester' and a['path'].startswith('/api/v1/tester') for a in r.body['apis']))
        self.assertEqual(len(ids), len(r.body['apis']))   # id 중복 없음
        args = HandlerArgs(method='GET', full_path='/api/v1/api-docs', client_ip='127.0.0.1', client_port=1, query_params={}, headers={})
        self.assertEqual(asyncio.run(H.handle_api_docs(args, {'config': _CFG})).status, 401)


class Events(unittest.TestCase):
    def test_sse_response(self):
        async def go():
            args = HandlerArgs(method='GET', full_path='/api/v1/tester/events', client_ip='127.0.0.1', client_port=1,
                               headers={'authorization': f'Bearer {_token("monitor")}'})
            r = await H.handle_tester(args, {'config': _CFG})
            self.assertIsNotNone(r.response)
            self.assertEqual(r.response.media_type, 'text/event-stream')
            it = r.response.body_iterator
            first = await it.__anext__()
            second = await it.__anext__()
            await it.aclose()
            return first, second
        first, second = asyncio.run(go())
        self.assertEqual(first, b': connected\n\n')
        self.assertIn(b'"stream": "hello"', second)


if __name__ == '__main__':
    unittest.main()
