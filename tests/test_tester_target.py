"""계측기 C 단계 단위시험 — 피어 풀: 번호 범위 펼치기·워커 고정·PoolCreate(peer)·대상 CSP 컬렉션 시드 파생/적용/복원.

Covers:
  - compile: e164_range 펼치기(접두·자릿수 보존, count 상한)·피어 풀은 bind.ip 호스트의 워커 하나에 고정(다른 워커 제외)·
    PoolCreate kind=peer(peer 문서·target_csp.peering 동봉)·bind.ip 워커 없음/서로 다른 워커 → CompileError
  - target: seed_pools(같은 route_set 형제 포함·seed.enabled=false 제외)·pick_local_node(수신점 → 기존 LocalNode 재사용/시드)·
    주소 사슬(listener.ip → node.addr → host.ip)·피어가 access 수신점을 가리키는 경우·이전 꼴 sip.access/peering 승계·
    derive_records(remote_node/route/rule/route_set/rule_set/routing_policy·ACL)·
    CspSeeder.apply/restore — 가짜 OAM(HTTP) 에 대해 GET 스냅샷 → 태그 잔재 제거+병합 PUT(마지막만 signal) → 원본 복원
  - driver: 피어 풀 시나리오 run 이 시드→풀→run→복원 순서로 완주(가짜 워커·가짜 OAM), invite expect.code=403 판정
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

try:
    import tests.test_tester_run as _TR  # noqa: E402
except ImportError:  # 디렉터리에서 직접 실행
    import test_tester_run as _TR  # noqa: E402
FakeWorker, _free_port = _TR.FakeWorker, _TR._free_port

S = M = R = C = T = None
_TMP = None


class FakeOam:
    """GET /api/v1/deployments · GET/PUT /api/v1/deployments/{id}/collection/{name} — 컬렉션은 메모리 dict."""

    def __init__(self):
        self.port = _free_port()
        self.collections = {c: [] for c in ('local_nodes', 'remote_nodes', 'routes', 'route_sets', 'rules', 'rule_sets',
                                            'routing_policies', 'acl_policies')}
        self.collections['local_nodes'] = [
            {'id': 'ln1', 'name': 'access-udp', 'enabled': True, 'is_primary': True, 'edge': 'access',
             'bind_ip': '10.0.0.1', 'bind_port': 5060, 'protocol': 'UDP'}]
        self.collections['remote_nodes'] = [
            {'id': 'stale', 'name': 'tester-rn-old', 'enabled': True, 'ip': '1.1.1.1', 'port': 5060, 'protocol': 'UDP',
             'tags': ['cims-tester']},
            {'id': 'keep', 'name': 'kt-sbc-1', 'enabled': True, 'ip': '2.2.2.2', 'port': 5060, 'protocol': 'UDP'}]
        self.puts = []
        self.extra = {}      # 경로(쿼리 제외) → 응답 body — 대상 관측 시험(agents metrics·recordings·alerts·events)
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, st, body):
                raw = json.dumps(body).encode()
                self.send_response(st)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):
                if self.headers.get('Authorization') != 'Bearer tok':
                    self._send(401, {'error': 'unauthorized'}); return
                if self.path.split('?')[0] in outer.extra:
                    body = outer.extra[self.path.split('?')[0]]
                    self._send(200, body() if callable(body) else body)
                elif self.path == '/api/v1/deployments':
                    self._send(200, {'items': [{'id': 7, 'package_name': 'oam', 'status': 'running', 'agent_id': 3, 'agent_name': 'mgmt'},
                                               {'id': 9, 'package_name': 'csp', 'status': 'running', 'agent_id': 5, 'agent_name': 'sut'}]})
                elif self.path.startswith('/api/v1/deployments/9/collection/'):
                    name = self.path.rsplit('/', 1)[-1]
                    self._send(200, {'records': outer.collections.get(name, []), 'schema': {}})
                else:
                    self._send(404, {'error': 'nf'})

            def do_PUT(self):
                n = int(self.headers.get('Content-Length') or 0)
                body = json.loads(self.rfile.read(n).decode()) if n else {}
                name = self.path.rsplit('/', 1)[-1]
                recs = []
                for i, r in enumerate(body.get('records') or []):
                    r = dict(r)
                    r.setdefault('id', f'auto{len(outer.puts)}-{i}')
                    recs.append(r)
                outer.collections[name] = recs
                outer.puts.append((name, body.get('signal', True), len(recs)))
                self._send(200, {'ok': True, 'count': len(recs), 'signaled': [1] if body.get('signal', True) else []})

        self.srv = HTTPServer(('127.0.0.1', self.port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.port}'


def setUpModule():
    global S, M, R, C, T, _TMP
    from services import tester_store as _S, tester_models as _M, tester_run as _R, tester_compile as _C, tester_target as _T
    S, M, R, C, T = _S, _M, _R, _C, _T
    _TR.S, _TR.M, _TR.R, _TR.C = S, M, R, C   # FakeWorker 가 계약 검증에 쓰는 모듈 전역
    _TMP = tempfile.mkdtemp(prefix='tester-peer-ut-')
    cfg = {'CimsRuntimeDir': os.path.join(_TMP, 'runtime'),
           'Tester': {'DataDir': os.path.join(_TMP, 'data'), 'WorkerStreamIp': '127.0.0.1',
                      'WorkerStreamPort': _free_port(), 'WorkerStreamAdvertiseIp': '127.0.0.1'}}
    from services import file_store, lease
    lease.acquire(file_store.runtime_root(cfg))
    S.init(_TESTER, cfg)
    R.RUNS.init(cfg)
    os.makedirs(os.path.join(S.user_scenarios_dir(), 'creds'), exist_ok=True)
    with open(os.path.join(S.user_scenarios_dir(), 'creds', 'ue.jsonl'), 'w') as f:
        for i in range(16):     # 워커마다 4 신원 — 풀이 source.offset 으로 구간을 나눠 쓴다
            f.write(json.dumps({'user': f'+8213000000{i:02d}', 'authId': f'450338213000000{i:02d}', 'ha1': 'ab' * 16}) + '\n')
    os.environ['UT_OAM_TOKEN'] = 'tok'


def tearDownModule():
    R.RUNS.shutdown()
    shutil.rmtree(_TMP, ignore_errors=True)


def _topology(workers, oam_url=None, peering=True, dead=True):
    """v2 — 대상 호스트 h1(10.0.0.1)에 csp 노드(수신점 udp access + peering), 워커 호스트 hw(127.0.0.1).
    UE 풀은 워커마다 `ue_<워커>` + group volte_ue, 피어 풀은 전부 첫 워커에(수신점 ip = hw). peering=False 면 피어 풀 없음."""
    nodes = {'csp': {'role': 'sip', 'host': 'h1', 'sip': {'domains': ['volte.test'],
                                                          'listeners': {'udp': {'edge': 'access', 'port': 5060, 'protocol': 'udp'}}}}}
    if peering:
        nodes['csp']['sip']['listeners']['peering'] = {'edge': 'peering', 'port': 5070, 'protocol': 'udp', 'local_node': 'cims-tester-peering'}
    if oam_url:
        port = int(oam_url.rsplit(':', 1)[-1])
        nodes['oam'] = {'role': 'oam', 'host': 'hw', 'oam': {'port': port, 'tls': False, 'token_env': 'UT_OAM_TOKEN'}}
    wrows = [{'name': w.name, 'host': 'hw', 'port': w.port, 'cpus': 1} for w in workers] or [{'name': 'w1', 'host': 'hw', 'port': 7100}]
    first = wrows[0]['name']
    pools = {f"ue_{w['name']}": {'kind': 'ue', 'worker': w['name'], 'group': 'volte_ue', 'access': 'csp',
                                 'source': {'creds': 'creds/ue.jsonl', 'offset': 4 * i, 'count': 4}} for i, w in enumerate(wrows)}
    if peering:
        pools.update({
            'peer_kt': {'kind': 'peer', 'worker': first, 'peering': 'csp', 'profile': 'ibcf', 'bind': {'port': 5080, 'protocol': 'udp'},
                        'domain': 'ims.kt.test', 'identities': {'e164_range': ['+82212340000', '+82212340019'], 'count': 5},
                        'seed': {'route_set': 'rs-kt', 'priority': 100}},
            'peer_blocked': {'kind': 'peer', 'worker': first, 'peering': 'csp', 'profile': 'ibcf', 'bind': {'port': 5082, 'protocol': 'udp'},
                             'domain': 'ims.blocked.test', 'identities': {'e164_range': ['+82299990000', '+82299990009']},
                             'seed': {'acl': 'deny'}},
            'peer_off': {'kind': 'peer', 'worker': first, 'peering': 'csp', 'profile': 'pbx', 'bind': {'port': 5090, 'protocol': 'udp'},
                         'domain': 'pbx.test', 'identities': {'did_range': ['0212345000', '0212345009']},
                         'seed': {'enabled': False}},
        })
        if dead:
            pools['peer_kt_dead'] = {'kind': 'peer', 'worker': first, 'peering': 'csp', 'profile': 'ibcf', 'bind': {'port': 5081, 'protocol': 'udp'},
                                     'domain': 'ims.kt.test', 'identities': {'e164_range': ['+82212340000', '+82212340009']},
                                     'answer': 'silent', 'seed': {'route_set': 'rs-kt', 'priority': 50}}
    return {'name': 'ut-peer', 'hosts': {'h1': {'ip': '10.0.0.1'}, 'hw': {'ip': '127.0.0.1'}}, 'workers': wrows,
            'target': {'name': 'sut', 'kind': 'cims', 'nodes': nodes}, 'pools': pools}


class CompilePeer(unittest.TestCase):
    def test_expand_range(self):
        self.assertEqual(C.expand_range('+8221234000', '+8221234002'), ['+8221234000', '+8221234001', '+8221234002'])
        self.assertEqual(C.expand_range('0009', '0011'), ['0009', '0010', '0011'])
        self.assertEqual(len(C.expand_range('1000', '1999', 10)), 10)
        with self.assertRaises(C.CompileError):
            C.expand_range('20', '10')

    def test_peer_pinned_to_bind_host_worker(self):
        w_far, w_near = FakeWorker('far'), FakeWorker('near')   # 피어 풀은 첫 워커(far)에 — 그 워커에 고정
        topo_doc = _topology([w_far, w_near])
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        for w in ws:
            w.probe()
        sc, _, _ = S.get_scenario('TRUNK-IBCF-OUTBOUND')
        plan = C.compile_run('r1', sc, topo, topo_doc, None, {}, ws, lambda w: '127.0.0.1:1', 2, None)
        self.assertEqual(list(plan['workers']), ['far'])          # 피어 풀 → 워커 하나에 고정
        self.assertEqual(plan['peer_pools'], ['peer_kt'])
        self.assertEqual(plan['identities']['peer_kt'], 5)         # count 상한
        pw = plan['workers']['far']
        peer_pc = [p for p in pw['pools'] if p['kind'] == 'peer'][0]
        self.assertEqual(peer_pc['pool'], 'peer_kt')
        self.assertEqual(peer_pc['peer']['bind']['port'], 5080)
        self.assertEqual(peer_pc['peer']['domain'], 'ims.kt.test')
        self.assertEqual(peer_pc['target_csp']['peering']['port'], 5070)
        self.assertEqual(peer_pc['identities'][0], {'user': '+82212340000', 'domain': 'ims.kt.test', 'auth_scheme': 'digest'})
        self.assertEqual(pw['run']['roles'], {'caller': 'ue_far', 'peer': 'peer_kt'})
        self.assertEqual(pw['run']['max_instances'], 2)
        self.assertEqual(peer_pc['peer']['bind']['ip'], '127.0.0.1')          # 수신점 ip = 워커 호스트(파생)
        self.assertNotIn('worker', peer_pc['peer'])                           # 워커 계약에는 토폴로지 참조 없음

    def test_peer_without_host_worker_rejected(self):
        # 피어 풀이 놓인 워커(w2)에 UE 역할의 로컬 풀이 없으면 후보 워커가 없다 → CompileError
        w1, w2 = FakeWorker('w1'), FakeWorker('w2')
        topo_doc = _topology([w1, w2])
        del topo_doc['pools']['ue_w2']
        for pn in ('peer_kt', 'peer_kt_dead', 'peer_blocked', 'peer_off'):
            topo_doc['pools'][pn]['worker'] = 'w2'
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        sc, _, _ = S.get_scenario('TRUNK-IBCF-OUTBOUND')
        with self.assertRaises(C.CompileError):
            C.compile_run('r1', sc, topo, topo_doc, None, {}, ws, lambda w: '127.0.0.1:1', 1, None)
        # 같은 수신점(호스트:포트)이 겹치면 토폴로지 검증 단계에서 거절
        bad = json.loads(json.dumps(topo_doc))
        bad['pools']['peer_blocked']['bind']['port'] = 5080
        _, errs = M.validate('topology', bad)
        self.assertTrue(any('겹친다' in e for e in errs), errs)

    def test_ue_only_scenario_not_pinned(self):
        w1, w2 = FakeWorker('w1'), FakeWorker('w2')
        topo_doc = _topology([w1, w2])
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        sc, _, _ = S.get_scenario('VOLTE-REGISTER')
        plan = C.compile_run('r1', sc, topo, topo_doc, None, {}, ws, lambda w: '127.0.0.1:1', 1, None)
        self.assertEqual(sorted(plan['workers']), ['w1', 'w2'])
        self.assertEqual(plan['peer_pools'], [])


class TokenAndDataDir(unittest.TestCase):
    def test_token_resolution_env_then_requester(self):
        """대상 OAM 토큰 — 환경변수가 먼저, 없으면 요청자 토큰(동거 형태), 둘 다 없으면 TargetError."""
        from services import tester_target as TT

        class O:
            token_env = 'UT_TESTER_OAM_TOKEN_X'
        os.environ.pop('UT_TESTER_OAM_TOKEN_X', None)
        self.assertEqual(TT.resolve_token(O(), 'req-token'), 'req-token')
        with self.assertRaises(TT.TargetError):
            TT.resolve_token(O(), None)
        os.environ['UT_TESTER_OAM_TOKEN_X'] = 'env-token'
        try:
            self.assertEqual(TT.resolve_token(O(), 'req-token'), 'env-token')
        finally:
            os.environ.pop('UT_TESTER_OAM_TOKEN_X', None)

    def test_default_data_dir_survives_upgrade(self):
        """배포 레이아웃(<모듈>/<버전>/<모듈>/ + <모듈>/runtime/)이면 기본 DataDir = runtime/data, 처음엔 버전 디렉터리 data 를 잇는다."""
        from services import tester_store as TS
        keep = (TS._component_root, TS._config, TS._data_dir_cache)
        root = tempfile.mkdtemp(prefix='ut-tester-layout-')
        try:
            comp = os.path.join(root, 'oam-cims-tester', '0.1.0', 'oam-cims-tester')
            os.makedirs(os.path.join(comp, 'data', 'scenarios', 'creds'))
            open(os.path.join(comp, 'data', 'scenarios', 'creds', 'volte.jsonl'), 'w').write('{}\n')
            TS._component_root, TS._config, TS._data_dir_cache = comp, {}, None
            self.assertEqual(TS.data_dir(), os.path.join(comp, 'data'))                  # runtime/ 없음 = 소스 트리 꼴
            os.makedirs(os.path.join(root, 'oam-cims-tester', 'runtime'))
            TS._data_dir_cache = None
            d = TS.data_dir()
            self.assertEqual(d, os.path.join(root, 'oam-cims-tester', 'runtime', 'data'))
            self.assertTrue(os.path.isfile(os.path.join(d, 'scenarios', 'creds', 'volte.jsonl')))   # 이어받음
            # 구버전(데이터가 버전 디렉터리 안)에서 올라온 첫 기동 — runtime/data 가 아직 없고 새 버전 data 는 빈 뼈대뿐
            shutil.rmtree(d)
            comp_new = os.path.join(root, 'oam-cims-tester', '0.2.0', 'oam-cims-tester')
            os.makedirs(os.path.join(comp_new, 'data', 'runs'))
            os.makedirs(os.path.join(comp_new, 'data', 'scenarios'))
            TS._component_root, TS._data_dir_cache = comp_new, None
            d = TS.data_dir()
            self.assertTrue(os.path.isfile(os.path.join(d, 'scenarios', 'creds', 'volte.jsonl')), '이전 버전 디렉터리의 data 를 이어받아야 한다')
            comp2 = os.path.join(root, 'oam-cims-tester', '0.1.1', 'oam-cims-tester')     # 업그레이드 — 새 버전 디렉터리
            os.makedirs(comp2)
            TS._component_root, TS._data_dir_cache = comp2, None
            self.assertEqual(TS.data_dir(), d)
            TS._config = {'Tester': {'DataDir': '/x/explicit'}}
            self.assertEqual(TS.data_dir(), '/x/explicit')                                # 명시값이 이긴다
        finally:
            TS._component_root, TS._config, TS._data_dir_cache = keep
            shutil.rmtree(root, ignore_errors=True)


class Observe(unittest.TestCase):
    def _topology(self, oam, observe):
        return M.Topology.model_validate({'name': 'ut-obs', 'hosts': {'h1': {'ip': '127.0.0.1'}},
            'workers': [{'name': 'w1', 'host': 'h1'}],
            'target': {'name': 'sut', 'kind': 'cims', 'nodes': {
                'csp': {'role': 'sip', 'host': 'h1', 'procs': ['csp'], 'sip': {'listeners': {'udp': {'edge': 'access', 'port': 5060}}, 'domains': ['x.test']}},
                'oam': {'role': 'oam', 'host': 'h1', 'oam': {'port': oam.port, 'tls': False, 'token_env': 'UT_OBS_TOKEN', 'observe': observe}}}},
            'pools': {'ue': {'kind': 'ue', 'worker': 'w1', 'access': 'csp', 'source': {'creds': 'c.jsonl'}}}})

    def test_observer_collects_host_cpu_and_evidence(self):
        from services import tester_observe as O
        from datetime import datetime
        oam = FakeOam()
        now = time.time()
        iso = lambda t: datetime.fromtimestamp(t).isoformat(timespec='seconds')
        # agent 5(csp 가 있는 호스트)만 관측 대상 — oam(7, agent 3)은 procs 에 없다. 최신이 앞(대상 OAM 규약)
        oam.extra['/api/v1/agents/5/metrics'] = lambda: {'items': [
            {'ts': iso(time.time()), 'cpu_pct': 91.0, 'mem_pct': 40.0, 'load_avg': '3.1,2,1'},
            {'ts': iso(time.time() - 3), 'cpu_pct': 89.0, 'mem_pct': 40.0, 'load_avg': '3.0,2,1'}]}
        oam.extra['/api/v1/recordings'] = {'recordings': [{'start_time': iso(now + 1)}, {'start_time': iso(now - 3600)}]}
        oam.extra['/api/v1/alerts'] = {'events': [{'ts': iso(now + 1), 'code': 'A-PRC-001', 'severity': 'major', 'action': 'raise'},
                                                  {'ts': iso(now + 1), 'code': 'A-PRC-001', 'severity': 'cleared', 'action': 'close'}]}
        oam.extra['/api/v1/events'] = {'events': [{'ts': iso(now + 2), 'code': 'E-STC-001'}]}
        os.environ['UT_OBS_TOKEN'] = 'tok'
        try:
            topo = self._topology(oam, ['agent_heartbeat'])
            self.assertTrue(O.TargetObserver.wanted(topo))
            self.assertFalse(O.TargetObserver.wanted(self._topology(oam, [])))
            db = os.path.join(_TMP, 'obs.sqlite')
            ob = O.TargetObserver('ut-obs', db, topo, None)
            ob.start()
            for _ in range(40):
                if ob.samples >= 1:
                    break
                time.sleep(0.1)
            cpu = ob.cpu_now()
            ob.stop(); ob.join(5)
            self.assertEqual(list(ob.agents), [5])
            self.assertIsNotNone(cpu)
            self.assertGreater(cpu, 85)
            self.assertEqual(list(O.target_series(db)['agents']), ['sut'])
            sc = M.Scenario.model_validate({'id': 'UT-EVID', 'roles': {'a': {'pool': 'ue'}}, 'flow': [{'step': 'register', 'who': ['a']}],
                'target_evidence': [{'kind': 'recording_created', 'min': 1}, {'kind': 'alarm_raised', 'code': 'A-PRC-001', 'max': 0},
                                    {'kind': 'event_logged', 'code': 'E-STC-001', 'min': 1}, {'kind': 'log_errors', 'max': 0}]})
            res = O.evaluate_evidence(sc, topo, now, now + 5, None)
            self.assertEqual([(r['kind'], r['observed'], r['ok']) for r in res],
                             [('recording_created', 1, True), ('alarm_raised', 1, False), ('event_logged', 1, True), ('log_errors', None, None)])
        finally:
            os.environ.pop('UT_OBS_TOKEN', None)
            oam.srv.shutdown()


class SshObservation(unittest.TestCase):
    """호스트 SSH 관측 — 가짜 실행기가 /proc 표본 두 장을 준다: 호스트 CPU 차분·프로세스 CPU(틱/CLK_TCK/경과)·RSS·log_errors(늘어난 ERROR 줄)."""

    def _topology(self):
        return M.Topology.model_validate({'name': 'ut-ssh', 'hosts': {'h1': {'ip': '10.0.0.9', 'ssh': {'user': 'cims', 'key_env': 'UT_SSH_KEY'}}},
            'workers': [{'name': 'w1', 'host': 'h1'}],
            'target': {'name': 'sut', 'kind': 'cims', 'nodes': {
                'csp': {'role': 'sip', 'host': 'h1', 'procs': ['csp'], 'logs': ['/opt/cims/csp/log/csp_*.log'],
                        'sip': {'listeners': {'udp': {'edge': 'access', 'port': 5060}}, 'domains': ['x.test']}}}},
            'pools': {'ue': {'kind': 'ue', 'worker': 'w1', 'access': 'csp', 'source': {'creds': 'c.jsonl'}}}})

    def test_ssh_observer_samples_and_log_errors(self):
        from services import tester_observe as O
        topo = self._topology()
        self.assertTrue(O.SshObserver.wanted(topo))
        calls = []
        samples = iter([
            '@T 1000.0 100\n@S cpu 1000 0 500 8000 100 0 0 0\n@M MemTotal: 8000000 kB\n@M MemAvailable: 4000000 kB\n@L 0.5\n@P csp 4242 300 102400 120\n@F 5000 /opt/cims/csp/log/csp_a.log\n',
            '@T 1003.0 100\n@S cpu 1300 0 600 8500 100 0 0 0\n@M MemTotal: 8000000 kB\n@M MemAvailable: 3000000 kB\n@L 0.9\n@P csp 4242 390 112640 133\n@F 5300 /opt/cims/csp/log/csp_a.log\n',
        ])

        def runner(host, cmd):
            calls.append(cmd)
            if '@N' in cmd:
                return '@N /opt/cims/csp/log/csp_a.log\n@N /opt/cims/csp/log/csp_b.log\n'
            if 'grep -c' in cmd:
                self.assertIn('tail -c +5001 /opt/cims/csp/log/csp_a.log', cmd)   # 시작 크기 다음 바이트부터
                self.assertIn('tail -c +1 /opt/cims/csp/log/csp_b.log', cmd)      # run 중 생긴 파일은 전체
                return '2\n1\n'
            return next(samples)

        db = os.path.join(_TMP, 'ssh.sqlite')
        ob = O.SshObserver('ut-ssh', db, topo, runner=runner, poll_s=0.05)
        ob.start()
        for _ in range(60):
            if ob.samples >= 1:
                break
            time.sleep(0.05)
        ob.stop(); ob.join(5)
        self.assertEqual(ob.samples, 1)
        # 호스트 CPU = (Δ전체 − Δidle)/Δ전체 = (900 − 500)/900 → 44.4 % · 프로세스 = 90 틱 / 100 / 3 s → 30 %
        self.assertAlmostEqual(ob.peak_cpu, 44.4, delta=0.2)
        self.assertAlmostEqual(ob.proc_peak['h1/csp'], 30.0, delta=0.1)
        self.assertAlmostEqual(ob.rss_delta_mb()['h1/csp'], 10.0, delta=0.1)
        self.assertEqual(ob.fd_delta(), {'h1/csp': 13})                        # 열린 fd 120 → 133
        self.assertEqual(ob.rss_slope_mb_per_h(), {})                          # 표본 2개·3 s — 기울기는 3개·10 s 부터
        self.assertAlmostEqual(ob.cpu_now(), 44.4, delta=0.2)
        ser = O.target_series(db)
        self.assertEqual(list(ser['agents']), ['h1'])
        self.assertEqual(list(ser['procs']), ['h1/csp'])
        self.assertEqual(ser['procs']['h1/csp']['fds'], [120, 133])
        self.assertAlmostEqual(ser['agents']['h1']['mem_pct'][0], 62.5, delta=0.1)
        self.assertEqual(ob.log_errors(), 3)
        sc = M.Scenario.model_validate({'id': 'UT-LOG', 'roles': {'a': {'pool': 'ue'}}, 'flow': [{'step': 'register', 'who': ['a']}],
                                        'target_evidence': [{'kind': 'log_errors', 'max': 0}]})
        res = O.evaluate_evidence(sc, topo, 0, 1, None, log_errors=3)
        self.assertEqual((res[0]['observed'], res[0]['ok']), (3, False))
        res = O.evaluate_evidence(sc, topo, 0, 1, None, log_errors=None)
        self.assertIsNone(res[0]['ok'])
        # 소크 누수 판정 — 프로세스별 RSS/fd 처음↔끝 차(proc 지정 = 이름 또는 host/proc, 비면 최댓값), 원천 없음·프로세스 없음 = 판정 불가
        sc2 = M.Scenario.model_validate({'id': 'UT-LEAK', 'roles': {'a': {'pool': 'ue'}}, 'flow': [{'step': 'register', 'who': ['a']}],
                                         'target_evidence': [{'kind': 'rss_growth_mb', 'proc': 'csp', 'max': 5}, {'kind': 'fd_growth', 'max': 20},
                                                             {'kind': 'rss_growth_mb', 'proc': 'cmp', 'max': 5}, {'kind': 'fd_growth', 'proc': 'h1/csp', 'max': 10}]})
        res = O.evaluate_evidence(sc2, topo, 0, 1, None, rss_delta={'h1/csp': 10.0, 'h1/cmp': 1.0}, fd_delta={'h1/csp': 13})
        self.assertEqual([(r['observed'], r['ok']) for r in res], [(10.0, False), (13, True), (1.0, True), (13, False)])
        self.assertIn('h1/csp +10 MB', res[0]['why'])
        res = O.evaluate_evidence(sc2, topo, 0, 1, None)                      # SSH 관측 없음 → 전부 판정 불가
        self.assertTrue(all(r['ok'] is None for r in res))
        for bad in ({'kind': 'log_errors', 'proc': 'csp', 'max': 0}, {'kind': 'rss_growth_mb', 'code': 'A-X', 'max': 1}, {'kind': 'fd_growth'}):
            with self.assertRaises(Exception):
                M.Evidence.model_validate(bad)
        # ssh argv — 개인키는 환경변수의 경로, 비밀은 레코드에 없다
        os.environ['UT_SSH_KEY'] = '/tmp/k'
        try:
            argv = O.ssh_argv(topo.hosts['h1'], 'true')
            self.assertIn('-i', argv); self.assertIn('/tmp/k', argv); self.assertEqual(argv[-2], 'cims@10.0.0.9')
        finally:
            os.environ.pop('UT_SSH_KEY', None)


class WorkerDiscovery(unittest.TestCase):
    def test_discover_deployed_workers_from_base_oam(self):
        """base OAM 배포 목록의 cims-tester-worker → 주소(agent ip)·포트(배포 설정 Server.Port, 없으면 7100)·cpus."""
        from services import tester_workers as TW
        oam = FakeOam()
        oam.extra['/api/v1/agents'] = {'items': [{'id': 5, 'name': 'gen-a', 'hostname': 'gena', 'ip_address': '10.0.0.61', 'cpu_cores': 8},
                                                 {'id': 6, 'name': 'gen-b', 'hostname': 'genb', 'ip_address': '', 'cpu_cores': 4,   # 주소 미기재 → 관리망 인터페이스
                                                  'interfaces': [{'name': 'e0', 'ip': '203.0.113.9'}, {'name': 'e1', 'ip': '10.0.0.62', 'mgmt': True}],
                                                  'routes': [{'dst': 'default', 'dev': 'e0', 'is_default': True}]}]}
        oam.extra['/api/v1/deployments'] = {'items': [
            {'id': 1, 'package_name': 'csp', 'agent_id': 5},
            {'id': 2, 'package_name': 'cims-tester-worker', 'agent_id': 5, 'agent_name': 'gen-a', 'package_version': '0.1.2', 'live_state': 'up', 'config': {}},
            {'id': 3, 'package_name': 'cims-tester-worker', 'agent_id': 6, 'agent_name': 'gen-b', 'package_version': '0.1.2', 'live_state': 'down',
             'config': {'Server.Port': 7105}}]}
        try:
            rows = TW.discover_deployed(oam.url, 'tok')
            self.assertEqual([(r['name'], r['ip'], r['port'], r['cpus'], r['live_state']) for r in rows],
                             [('gen-a', '10.0.0.61', 7100, 8, 'up'), ('gen-b', '10.0.0.62', 7105, 4, 'down')])
            with self.assertRaises(T.TargetError):
                TW.discover_deployed(oam.url, 'wrong-token')
        finally:
            oam.srv.shutdown()


class SeedDerivation(unittest.TestCase):
    def _topo(self, **kw):
        return M.Topology.model_validate(_topology([], **kw))

    def test_seed_pools_declared_only(self):
        t = self._topo()
        pools = T.seed_pools(t, {'volte_ue', 'peer_kt'})
        self.assertEqual(sorted(pools), ['peer_kt'])                     # 같은 route_set 형제라도 역할로 선언해야 시드
        self.assertEqual(sorted(T.seed_pools(t, {'peer_kt', 'peer_kt_dead'})), ['peer_kt', 'peer_kt_dead'])
        self.assertEqual(sorted(T.seed_pools(t, {'peer_off'})), [])     # seed.enabled=false
        self.assertEqual(sorted(T.seed_pools(t, {'peer_blocked'})), ['peer_blocked'])

    def test_pick_local_node(self):
        t = self._topo()
        # ① 항목 local_node 이름이 대상에 없고 같은 포트 레코드도 없음 → 항목 그대로(edge 포함) 시드
        ref, new = T.pick_local_node(t, [{'name': 'access-udp', 'bind_port': 5060, 'protocol': 'UDP', 'is_primary': True}], 'csp', 'peering')
        self.assertEqual(ref, 'cims-tester-peering')
        self.assertEqual((new['edge'], new['bind_ip'], new['bind_port'], new['protocol']), ('peering', '10.0.0.1', 5070, 'UDP'))
        # ② 이름이 있으면 재사용
        ref2, new2 = T.pick_local_node(t, [{'name': 'cims-tester-peering', 'bind_port': 5070, 'protocol': 'UDP'}], 'csp', 'peering')
        self.assertEqual((ref2, new2), ('cims-tester-peering', None))
        # ③ 피어가 access 수신점을 가리키면 같은 protocol·port 의 기존 레코드(bind_ip 0.0.0.0 도)를 접속점으로 — 새 LocalNode 없음
        ref3, new3 = T.pick_local_node(t, [{'name': 'access-udp', 'bind_port': 5060, 'protocol': 'UDP', 'bind_ip': '0.0.0.0'}], 'csp', 'udp')
        self.assertEqual((ref3, new3), ('access-udp', None))
        # ④ 그 포트 레코드가 없으면 access edge 로 시드(local_node 비면 cims-tester-<id>)
        ref4, new4 = T.pick_local_node(t, [], 'csp', 'udp')
        self.assertEqual((ref4, new4['edge'], new4['bind_port']), ('cims-tester-udp', 'access', 5060))

    def test_listener_address_chain_and_peer_on_access(self):
        """주소 사슬 listener.ip → node.addr → host.ip · 피어 풀이 access 수신점을 가리켜도 된다(CSP 는 Route 로 신뢰)."""
        doc = _topology([FakeWorker('w1')])
        csp = doc['target']['nodes']['csp']
        csp['addr'] = '10.0.0.99'                                                       # VIP
        csp['sip']['listeners']['tls2'] = {'edge': 'access', 'ip': '10.0.0.98', 'port': 5061, 'protocol': 'tls'}
        doc['pools']['peer_kt']['listener'] = 'udp'                                     # 피어 → access UDP 접속점
        doc['pools']['peer_kt']['bind']['ip'] = '127.0.0.2'                             # 워커 호스트의 다른 IP
        doc['pools']['ue_w1']['listener'] = 'tls2'                                      # transport 는 수신점에서
        t = M.Topology.model_validate(doc)
        self.assertEqual((t.node_ip('csp'), t.listener_ip('csp', 'udp'), t.listener_ip('csp', 'tls2')), ('10.0.0.99', '10.0.0.99', '10.0.0.98'))
        self.assertEqual(t.pools['ue_w1'].transport, 'tls')
        self.assertEqual(t.target_csp_for('ue_w1').ip, '10.0.0.98')
        self.assertEqual(t.pool_bind_ip('peer_kt'), '127.0.0.2')
        nid, lid, l = t.pool_listener('peer_kt')
        self.assertEqual((nid, lid, l.edge), ('csp', 'udp', 'access'))
        tc = t.target_csp_for('peer_kt')
        self.assertEqual((tc.ip, tc.peering.ip, tc.peering.port, tc.peering.local_node), ('10.0.0.99', '10.0.0.99', 5060, 'cims-tester-udp'))
        self.assertEqual(t.pool_listener('peer_blocked')[1], 'peering')               # listener 없으면 edge=peering 첫 항목
        # 시드: 접속점은 기존 access-udp 레코드, ACL 은 그 피어의 Route 에만
        recs = T.derive_records(t, {'peer_kt': t.pools['peer_kt']}, {'peer_kt': 'access-udp'})
        self.assertEqual(recs['routes'][0]['local_node_ref'], 'access-udp')
        self.assertEqual(recs['routes'][0]['inbound_auth'], 'none')
        self.assertEqual(recs['remote_nodes'][0]['ip'], '127.0.0.2')
        # 검증: transport 와 수신점 protocol 이 어긋나면 오류 · 수신점 튜플 중복 오류 · 이전 꼴 access/peering 은 승계
        bad = dict(doc); bad['pools'] = dict(doc['pools']); bad['pools']['ue_w1'] = dict(doc['pools']['ue_w1'], transport='udp')
        self.assertTrue(any('tls' in e for e in M.validate('topology', bad)[1]))
        dup = _topology([FakeWorker('w1')]); dup['target']['nodes']['csp']['sip']['listeners']['udp2'] = {'edge': 'access', 'port': 5060, 'protocol': 'udp'}
        self.assertTrue(any('겹친다' in e for e in M.validate('topology', dup)[1]))
        legacy = _topology([FakeWorker('w1')])
        legacy['target']['nodes']['csp']['sip'] = {'access': {'udp': 5060, 'tls': 5061, 'domains': ['volte.test']},
                                                   'peering': {'port': 5070, 'protocol': 'udp', 'local_node': 'cims-tester-peering'}}
        lt = M.Topology.model_validate(legacy)
        self.assertEqual(sorted(lt.target.nodes['csp'].sip.listeners), ['peering', 'tls', 'udp'])
        self.assertEqual(lt.target.nodes['csp'].sip.listeners['peering'].edge, 'peering')
        self.assertEqual(lt.domains_of('csp'), ['volte.test'])
        self.assertEqual(M.normalize_topology_doc(legacy)['target']['nodes']['csp']['sip']['listeners']['tls'], {'edge': 'access', 'port': 5061, 'protocol': 'tls'})

    def test_derive_records(self):
        t = self._topo()
        recs = T.derive_records(t, T.seed_pools(t, {'peer_kt', 'peer_kt_dead', 'peer_blocked'}), {p: 'ln-x' for p in ('peer_kt', 'peer_kt_dead', 'peer_blocked')})
        names = {c: [r['name'] for r in v] for c, v in recs.items()}
        self.assertEqual(sorted(names['remote_nodes']), ['tester-rn-peer_blocked', 'tester-rn-peer_kt', 'tester-rn-peer_kt_dead'])
        self.assertTrue(all(r['local_node_ref'] == 'ln-x' for r in recs['routes']))
        rs = {r['name']: r for r in recs['route_sets']}
        self.assertEqual(sorted(rs), ['tester-rs-peer_blocked', 'tester-rs-rs-kt'])
        kt = rs['tester-rs-rs-kt']
        self.assertEqual(kt['distribution_policy'], 'failover')
        self.assertEqual([m['route_ref'] for m in kt['members']], ['tester-r-peer_kt_dead', 'tester-r-peer_kt'])   # priority 순
        self.assertEqual(kt['health_check_mode'], 'none')
        rules = {r['name']: r for r in recs['rules']}
        self.assertEqual(rules['tester-rule-peer_kt-domain'], {'name': 'tester-rule-peer_kt-domain', 'enabled': True,
                                                               'field': 'req_uri_host', 'op': 'eq', 'value': 'ims.kt.test',
                                                               'tags': ['cims-tester', 'routing']})
        self.assertEqual(rules['tester-rule-peer_blocked-src']['value'], '127.0.0.1')
        acl = recs['acl_policies'][0]
        self.assertEqual((acl['action'], acl['scope'], acl['scope_ref'], acl['match_rule_set_ref']),
                         ('deny', 'route', 'tester-r-peer_blocked', 'tester-rs-peer_blocked-src'))   # 그 피어의 Route 에만
        self.assertTrue(all(r['inbound_auth'] == 'none' for r in recs['routes']))
        rp = {r['name']: r for r in recs['routing_policies']}
        self.assertEqual(rp['tester-rp-rs-kt']['target_ref'], 'tester-rs-rs-kt')
        self.assertTrue(all('cims-tester' in r['tags'] for v in recs.values() for r in v))

    def test_seeder_apply_restore(self):
        oam = FakeOam()
        t = self._topo(oam_url=oam.url)
        seeder = T.CspSeeder.for_run(t, {'peer_kt'})
        self.assertEqual(seeder.dep_id, 9)                             # 패키지 csp 자동 발견
        applied = seeder.apply()
        self.assertEqual(seeder.local_node_ref, 'cims-tester-peering')
        self.assertEqual(applied['local_nodes'], 1)
        self.assertEqual(applied['remote_nodes'], 1)
        rn = oam.collections['remote_nodes']
        self.assertEqual(sorted(r['name'] for r in rn), ['kt-sbc-1', 'tester-rn-peer_kt'])   # 잔재 제거·원본 유지
        self.assertEqual([r['name'] for r in oam.collections['local_nodes']], ['access-udp', 'cims-tester-peering'])
        signals = [s for _, s, _ in oam.puts]
        self.assertEqual(signals.count(True), 1)
        self.assertTrue(signals[-1])                                    # 마지막 PUT 만 SIGUSR1
        n_apply = len(oam.puts)
        errs = seeder.restore()
        self.assertEqual(errs, [])
        self.assertEqual([r['name'] for r in oam.collections['remote_nodes']], ['tester-rn-old', 'kt-sbc-1'])   # 스냅샷 그대로
        self.assertEqual([r['name'] for r in oam.collections['local_nodes']], ['access-udp'])
        self.assertEqual(oam.collections['routing_policies'], [])
        self.assertEqual([s for _, s, _ in oam.puts[n_apply:]].count(True), 1)

    def test_seeder_requires_oam_and_token(self):
        t = self._topo()
        with self.assertRaises(T.TargetError):
            T.CspSeeder.for_run(t, {'peer_kt'})
        self.assertIsNone(T.CspSeeder.for_run(t, {'peer_off'}))
        oam = FakeOam()
        t2 = self._topo(oam_url=oam.url)
        os.environ['UT_OAM_TOKEN'] = ''
        try:
            with self.assertRaises(T.TargetError):
                T.CspSeeder.for_run(t2, {'peer_kt'})
        finally:
            os.environ['UT_OAM_TOKEN'] = 'tok'


class PbxMgcf(unittest.TestCase):
    """D 단계 — 번호 접두 규칙·트렁크 REGISTER 비밀 해석·피어 register 역할 검사."""

    def _topo(self):
        w = FakeWorker('w1')
        doc = _topology([w])
        del doc['pools']['peer_off']   # 5090 수신점을 pbx_hq 가 쓴다
        doc['pools']['pbx_hq'] = {'kind': 'peer', 'worker': 'w1', 'peering': 'csp', 'profile': 'pbx', 'bind': {'port': 5090, 'protocol': 'udp'},
                                  'domain': 'pbx.hq.test', 'register': {'user': 'pbx-hq', 'ha1_env': 'UT_PBX_HA1'},
                                  'identities': {'did_range': ['0212345000', '0212345099']}, 'codecs': ['PCMA', 'PCMU']}
        doc['pools']['mgcf_pstn'] = {'kind': 'peer', 'worker': 'w1', 'peering': 'csp', 'profile': 'mgcf', 'bind': {'port': 5095, 'protocol': 'udp'},
                                     'domain': 'mgcf.pstn.test', 'identities': {'e164_range': ['+82312340000', '+82312340099']}}
        return w, doc, M.Topology.model_validate(doc)

    def test_number_prefix_rule(self):
        _, _, topo = self._topo()
        self.assertEqual(T.number_prefix(topo.pools['pbx_hq']), '02123450')
        self.assertEqual(T.number_prefix(topo.pools['mgcf_pstn']), '+823123400')
        recs = T.derive_records(topo, {'pbx_hq': topo.pools['pbx_hq'], 'mgcf_pstn': topo.pools['mgcf_pstn'],
                                       'peer_kt': topo.pools['peer_kt']}, {p: 'cims-tester-peering' for p in ('pbx_hq', 'mgcf_pstn', 'peer_kt')})
        names = {r['name']: r for r in recs['rules']}
        self.assertEqual(names['tester-rule-pbx_hq-prefix']['field'], 'req_uri_user')
        self.assertEqual(names['tester-rule-pbx_hq-prefix']['op'], 'prefix')
        self.assertEqual(names['tester-rule-mgcf_pstn-prefix']['value'], '+823123400')
        self.assertNotIn('tester-rule-peer_kt-prefix', names)   # ibcf 는 도메인만
        match = {r['name']: r for r in recs['rule_sets']}['tester-rs-mgcf_pstn-match']
        self.assertEqual([m['rule_ref'] for m in match['members']], ['tester-rule-mgcf_pstn-domain', 'tester-rule-mgcf_pstn-prefix'])

    def test_trunk_register_secret(self):
        _, _, topo = self._topo()
        os.environ.pop('UT_PBX_HA1', None)
        with self.assertRaises(C.CompileError):
            C.trunk_register_for('pbx_hq', topo.pools['pbx_hq'], 'volte.test')
        os.environ['UT_PBX_HA1'] = 'cd' * 16
        tr = C.trunk_register_for('pbx_hq', topo.pools['pbx_hq'], 'volte.test')
        self.assertEqual((tr.user, tr.ha1, tr.expires, tr.realm), ('pbx-hq', 'cd' * 16, 3600, 'volte.test'))
        self.assertIsNone(C.trunk_register_for('mgcf_pstn', topo.pools['mgcf_pstn'], None))

    def test_trunk_register_seed_route(self):
        """pbx register → 시드 Route 가 등록형 트렁크 계정(inbound_auth=digest + auth_user/auth_ha1/auth_realm/register_expires)이 된다.
        비밀 환경변수가 없으면 시드도 컴파일과 같은 오류로 멈춘다(조용히 none 으로 시드하지 않는다)."""
        _, _, topo = self._topo()
        os.environ.pop('UT_PBX_HA1', None)
        with self.assertRaises(C.CompileError):
            T.derive_records(topo, {'pbx_hq': topo.pools['pbx_hq']}, {'pbx_hq': 'ln-x'})
        os.environ['UT_PBX_HA1'] = 'cd' * 16
        try:
            recs = T.derive_records(topo, {'pbx_hq': topo.pools['pbx_hq'], 'mgcf_pstn': topo.pools['mgcf_pstn']}, {'pbx_hq': 'ln-x', 'mgcf_pstn': 'ln-x'})
        finally:
            os.environ.pop('UT_PBX_HA1', None)
        routes = {r['name']: r for r in recs['routes']}
        pbx = routes['tester-r-pbx_hq']
        self.assertEqual((pbx['inbound_auth'], pbx['auth_user'], pbx['auth_ha1'], pbx['auth_realm'], pbx['register_expires']),
                         ('digest', 'pbx-hq', 'cd' * 16, '', 3600))
        self.assertEqual(routes['tester-r-mgcf_pstn']['inbound_auth'], 'none')      # 고정 IP 피어는 그대로 신뢰 피어
        self.assertNotIn('auth_user', routes['tester-r-mgcf_pstn'])
        # pbx RemoteNode 는 G.711 삽입 코덱(트랜스코딩 정책, cmp.md §11.2) — mgcf 는 IM-MGW 가 AMR-WB 를 오퍼하므로 없음
        rns = {r['name']: r for r in recs['remote_nodes']}
        self.assertEqual(rns['tester-rn-pbx_hq']['transcode_codecs'], ['PCMA', 'PCMU'])
        self.assertNotIn('transcode_codecs', rns['tester-rn-mgcf_pstn'])

    def test_register_role_on_peer_requires_trunk(self):
        _, _, topo = self._topo()
        sc = M.Scenario.model_validate({'id': 'UT-REG', 'roles': {'m': {'pool': 'mgcf_pstn'}, 'u': {'pool': 'volte_ue'}},
                                        'flow': [{'step': 'register', 'who': ['m', 'u']}, {'step': 'invite', 'from': 'm', 'to': 'u'}]})
        with self.assertRaises(C.CompileError):
            C.check_register_roles(sc, topo, {'m': 'mgcf_pstn', 'u': 'ue_w1'})
        sc2 = M.Scenario.model_validate({'id': 'UT-REG2', 'roles': {'p': {'pool': 'pbx_hq'}, 'u': {'pool': 'volte_ue'}},
                                         'flow': [{'step': 'register', 'who': ['p', 'u']}, {'step': 'invite', 'from': 'p', 'to': 'u'}]})
        C.check_register_roles(sc2, topo, {'p': 'pbx_hq', 'u': 'ue_w1'})   # 트렁크 계정 있음 — 통과
        # kind 게이트 — progress 는 피어와 착신 UE 둘 다(UE 측 183 early media), replaces 는 UE 만
        sc3 = M.Scenario.model_validate({'id': 'UT-GATE', 'roles': {'u': {'pool': 'volte_ue'}, 'p': {'pool': 'pbx_hq'}},
                                         'flow': [{'step': 'invite', 'from': 'p', 'to': 'u'}, {'step': 'progress', 'who': ['u']}]})
        C.check_kind_gates(sc3, topo, {'u': 'ue_w1', 'p': 'pbx_hq'})
        sc4 = M.Scenario.model_validate({'id': 'UT-GATE2', 'roles': {'u': {'pool': 'volte_ue'}, 'p': {'pool': 'pbx_hq'}},
                                         'flow': [{'step': 'subscribe', 'who': ['p'], 'to': 'u'}, {'step': 'invite', 'from': 'u', 'to': 'p'},
                                                  {'step': 'replaces', 'from': 'p', 'to': 'u'}]})
        with self.assertRaises(C.CompileError):
            C.check_kind_gates(sc4, topo, {'u': 'ue_w1', 'p': 'pbx_hq'})

    def test_compile_pool_create_carries_options(self):
        w, doc, topo = self._topo()
        os.environ['UT_PBX_HA1'] = 'cd' * 16
        doc['pools']['ue_w1']['prack'] = True
        doc['target']['nodes']['csp']['sip']['listeners']['tls'] = {'edge': 'access', 'port': 5061, 'protocol': 'tls'}
        doc['pools']['ue_w1']['dtmf'] = 'inband'
        doc['pools']['ue_w1']['transport'] = 'tls'
        doc['pools']['ue_w1']['tls_verify'] = True
        doc['pools']['pbx_hq']['fault'] = {'drop_invite': 1}
        doc['pools']['pbx_hq']['dtmf'] = False
        doc['pools']['pbx_hq']['tls_client_cert'] = True
        topo = M.Topology.model_validate(doc)
        sc, _, _ = S.get_scenario('TRUNK-PBX-REGISTER')
        from services import tester_workers as TW
        ws = TW.discover(doc)
        for x in ws:
            x.probe()
        plan = C.compile_run('r-pbx', sc, topo, doc, None, {}, ws, lambda w: '127.0.0.1:1', 1, None)
        pools = {p['pool']: p for p in plan['workers']['w1']['pools']}
        self.assertEqual(pools['pbx_hq']['trunk_register']['ha1'], 'cd' * 16)
        self.assertTrue(pools['ue_w1']['prack'])
        self.assertEqual((pools['ue_w1']['dtmf'], pools['ue_w1']['tls_verify'], pools['ue_w1']['tls_client_cert']), ('inband', True, False))
        self.assertEqual((pools['pbx_hq']['peer']['fault']['drop_invite'], pools['pbx_hq']['peer']['dtmf'], pools['pbx_hq']['peer']['thig']), (1, 'off', False))
        self.assertTrue(pools['pbx_hq']['tls_client_cert'] and pools['pbx_hq']['peer']['tls_client_cert'])
        self.assertEqual(pools['pbx_hq']['trunk_register']['realm'], 'volte.test')   # 비면 접속점 기본 도메인
        steps = {s['step']: s for s in plan['steps']}
        self.assertIn('register', steps)
        sc2, _, _ = S.get_scenario('TRUNK-MGCF-OUTBOUND')
        doc2 = json.loads(json.dumps(doc))
        plan2 = C.compile_run('r-mgcf', sc2, topo, doc2, None, {}, ws, lambda w: '127.0.0.1:1', 1, None)
        bye = [s for s in plan2['steps'] if s['step'] == 'bye'][0]
        self.assertEqual(bye['cause'], 16)
        self.assertEqual([s['step'] for s in plan2['steps']], ['register', 'invite', 'progress', 'answer', 'media_hold', 'bye'])


class DriverPeer(unittest.TestCase):
    def _run(self, scenario_id, oam, flow=None, instances=2):
        w = FakeWorker('w1')
        topo_doc = _topology([w], oam_url=oam.url if oam else None)
        rec, errs = S.save_topology(topo_doc)
        self.assertFalse(errs, errs)
        sid = scenario_id
        if flow is not None:
            _, doc, _ = S.get_scenario(scenario_id)
            doc = json.loads(json.dumps(doc))
            doc['id'] = 'UT-' + scenario_id
            doc['flow'] = flow
            import yaml
            with open(os.path.join(S.user_scenarios_dir(), 'ut-peer.yaml'), 'w') as f:
                yaml.safe_dump(doc, f, allow_unicode=True)
            sid = doc['id']
        d = R.RUNS.start(M.RunRequest(scenario_id=sid, topology_id=rec['id'], instances=instances))
        d.join(30)
        return d, w

    def test_outbound_seeds_then_runs_then_restores(self):
        oam = FakeOam()
        d, w = self._run('TRUNK-IBCF-OUTBOUND', oam)
        self.assertEqual(d.state, 'stopped')
        self.assertEqual(d.verdict, 'pass', d.notes + d.expect_results)
        self.assertTrue(any(n.startswith('csp seed(dep 9') for n in d.notes), d.notes)
        self.assertIn('csp seed restored', d.notes)
        kinds = sorted(p['kind'] for p in w.pools)
        self.assertEqual(kinds, ['peer', 'ue'])
        # 시드 PUT 이 풀 생성보다 먼저, 복원 PUT 이 run 종료 뒤 — 복원 뒤 컬렉션은 원본
        self.assertEqual([r['name'] for r in oam.collections['remote_nodes']], ['tester-rn-old', 'kt-sbc-1'])

    def test_acl_deny_expects_403(self):
        oam = FakeOam()
        # 가짜 워커는 성공 agg 만 보낸다 → 403 이 관측되지 않으므로 fail 이어야 한다(판정 로직)
        d, _ = self._run('TRUNK-IBCF-ACL-DENY', oam)
        self.assertEqual(d.verdict, 'fail')
        r = [x for x in d.expect_results if x['metric'] == 'code' and x['kind'] == 'invite'][0]
        self.assertFalse(r['ok'])
        self.assertIn('codes.403=0', r['observed'])

    def test_missing_oam_is_error(self):
        d, _ = self._run('TRUNK-IBCF-OUTBOUND', None)
        self.assertEqual(d.verdict, 'error')
        self.assertTrue(any('oam' in n for n in d.notes), d.notes)


if __name__ == '__main__':
    unittest.main()
